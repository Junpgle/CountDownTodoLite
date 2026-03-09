/**
 * pomodoro_window.cpp
 * 番茄钟专注工作台
 * 功能：计时、标签、绑定待办、历史统计、云端同步
 */
#include "pomodoro_window.h"
#include "pomodoro_stats_window.h"
#include "ws_pomodoro.h"
#include "common.h"
#include "utils.h"
#include "api.h"
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
using namespace Gdiplus;
// ============================================================
// 颜色常量
// ============================================================
static const Color P_BG      = Color(255, 245, 247, 250);
static const Color P_ACCENT  = Color(255, 239,  83,  80);   // 番茄红
static const Color P_GREEN   = Color(255,  52, 199, 120);   // 休息绿
static const Color P_TEXT    = Color(255,  30,  40,  60);
static const Color P_SUB     = Color(255, 130, 140, 160);
static const Color P_BORDER  = Color(255, 220, 225, 235);
static const Color P_HOVER   = Color(255, 235, 238, 248);
static const Color P_IDLE    = Color(255,  74, 108, 247);   // 待机蓝
// ============================================================
// 窗口内部状态
// ============================================================
static HWND  s_hWnd    = NULL;
static HWND  s_hParent = NULL;
static const UINT_PTR TIMER_ID_TICK = 3001;
// HitZone
struct PHitZone { Gdiplus::Rect rect; int id; };
static std::vector<PHitZone> s_Hz;
// HitId 枚举
enum PHitId {
    PH_BTN_START    = 1,
    PH_BTN_STOP     = 2,
    PH_BTN_SKIP     = 3,
    PH_LOOP_DEC     = 4,
    PH_LOOP_INC     = 5,
    PH_FOCUS_DEC    = 6,
    PH_FOCUS_INC    = 7,
    PH_REST_DEC     = 8,
    PH_REST_INC     = 9,
    PH_TAG_BASE     = 100,  // +i => 标签 i
    PH_TODO_BASE    = 200,  // +i => 待办 i
    PH_HIST_REFRESH = 300,
    PH_STATS_OPEN   = 301,
    PH_CLOSE        = 999,
};
// 当前选中的标签 uuid 列表（可多选）
static std::vector<std::wstring> s_SelTags;
// 绑定的待办 uuid
static std::wstring s_BoundTodo;
// hitId -> 待办 uuid 映射（绑定待办区专用，避免因 g_Todos 顺序变化导致 idx 错位）
static std::map<int, std::wstring> s_TodoHitUuid;
// 历史分页
static int s_HistPage = 0;
static const int HIST_PER_PAGE = 5;
// 滚动支持
static int s_ScrollY   = 0;
static int s_ContentH  = 0;
static int s_WinH      = 0;
// ============================================================
// 辅助：生成 UUID
// ============================================================
static std::wstring GenUuid() {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)(time(nullptr) ^ (uintptr_t)GetCurrentProcessId()));
        seeded = true;
    }
    wchar_t buf[48];
    swprintf_s(buf, L"pom%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
        rand()&0xFFFF, rand()&0xFFFF, rand()&0xFFFF, rand()&0xFFFF,
        rand()&0xFFFF, rand()&0xFFFF, rand()&0xFFFF, rand()&0xFFFF);
    return buf;
}
// ============================================================
// 辅助：秒数格式化 MM:SS
// ============================================================
static std::wstring FmtCountdown(int secs) {
    if (secs < 0) secs = 0;
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", secs / 60, secs % 60);
    return buf;
}
// ============================================================
// 辅助：计算剩余秒数
// ============================================================
static int CalcRemain() {
    auto& s = g_PomodoroSession;
    if (s.status == PomodoroStatus::Idle) return 0;
    long long nowMs = (long long)time(nullptr) * 1000LL;
    long long diff  = s.targetEndMs - nowMs;
    return (diff > 0) ? (int)(diff / 1000) : 0;
}
// ============================================================
// 辅助：结束当前专注记录并上传
// ============================================================
static void FinishCurrentRecord(const std::wstring& statusStr) {
    auto& s = g_PomodoroSession;
    if (s.currentRecordUuid.empty()) return;
    long long nowMs = (long long)time(nullptr) * 1000LL;
    long long startMs = s.targetEndMs - (long long)s.focusDuration * 1000LL;
    int actual = (int)((nowMs - startMs) / 1000);
    if (actual < 0) actual = 0;
    PomodoroRecord rec;
    rec.uuid            = s.currentRecordUuid;
    rec.todoUuid        = s.boundTodoUuid;
    rec.startTime       = startMs;
    rec.endTime         = nowMs;
    rec.plannedDuration = s.focusDuration;
    rec.actualDuration  = actual;
    rec.status          = statusStr;
    rec.deviceId        = g_DeviceId;
    rec.version         = 1;
    rec.createdAt       = startMs;
    rec.updatedAt       = nowMs;
    // 加入本地历史（供统计显示）并立即持久化
    {
        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        // 标记为待上传
        rec.isDirty = true;
        g_PomodoroHistory.insert(g_PomodoroHistory.begin(), rec);
        if (g_PomodoroHistory.size() > 500) g_PomodoroHistory.resize(500);
    }
    // 先同步写本地缓存（保证断网时不丢失）
    SavePomodoroLocalCache();
    // 再异步上传云端（成功后 isDirty 置 false 并再次保存缓存）
    std::thread([rec]() {
        if (ApiUploadPomodoroRecord(rec)) {
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            for (auto& r : g_PomodoroHistory) {
                if (r.uuid == rec.uuid) { r.isDirty = false; break; }
            }
            SavePomodoroLocalCache();
        }
    }).detach();
    s.currentRecordUuid.clear();
}
// ============================================================
// 核心绘制
// ============================================================
static void PomodoroRender() {
    if (!s_hWnd) return;
    RECT rc; GetClientRect(s_hWnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    s_WinH = H;

    HDC hdcWin = GetDC(s_hWnd);

    // 离屏大画布（足够容纳所有内容）
    const int CANVAS_H = 2400;
    HDC hdcCanvas = CreateCompatibleDC(hdcWin);
    HBITMAP hCanvasBmp = CreateCompatibleBitmap(hdcWin, W, CANVAS_H);
    HBITMAP hCanvasOld = (HBITMAP)SelectObject(hdcCanvas, hCanvasBmp);

    Graphics g(hdcCanvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    // 背景填满大画布
    SolidBrush bgBr(P_BG);
    g.FillRectangle(&bgBr, 0, 0, W, CANVAS_H);

    s_Hz.clear();

    FontFamily& ff = *g_MiSansFamily;
    auto MkFont = [&](float sz, int style = FontStyleRegular) {
        return new Font(&ff, sz, style, UnitPixel);
    };
    SolidBrush txBr(P_TEXT), subBr(P_SUB), wBr(Color(255,255,255,255));
    StringFormat sfC, sfL, sfR;
    sfC.SetAlignment(StringAlignmentCenter);
    sfC.SetLineAlignment(StringAlignmentCenter);
    sfL.SetAlignment(StringAlignmentNear);
    sfL.SetLineAlignment(StringAlignmentCenter);
    sfL.SetTrimming(StringTrimmingEllipsisCharacter);
    sfL.SetFormatFlags(StringFormatFlagsNoWrap);
    sfR.SetAlignment(StringAlignmentFar);
    sfR.SetLineAlignment(StringAlignmentCenter);

    auto& ses = g_PomodoroSession;
    int remain  = CalcRemain();
    bool isIdle = (ses.status == PomodoroStatus::Idle);
    bool isRest = ses.isRestPhase;

    // ── 标题行（固定在顶部，不随滚动偏移）──────────────────
    float y = 18.0f;
    {
        Font* fTitle = MkFont(20, FontStyleBold);
        SolidBrush acBr(P_ACCENT);
        g.DrawString(L"番茄钟", -1, fTitle, RectF(0, y, (REAL)W, 32), &sfC, &acBr);
        delete fTitle;
        Font* fClose = MkFont(14);
        SolidBrush closeBr(P_SUB);
        g.DrawString(L"X", -1, fClose, RectF((REAL)(W-40), y, 30, 28), &sfC, &closeBr);
        // HitZone 坐标对应屏幕坐标（标题行固定，不偏移）
        s_Hz.push_back({Gdiplus::Rect(W-40,(int)y,30,28), PH_CLOSE});
        delete fClose;
    }
    y += 48.0f;

    // 从这里开始的内容都受 s_ScrollY 影响

    // ── 计时器大圆盘 ────────────────────────────────────────
    {
        int cx = W / 2;
        int cy = (int)y + 100;
        int R  = 90;
        Pen trackPen(P_BORDER, 10.0f);
        g.DrawEllipse(&trackPen, cx-R, cy-R, R*2, R*2);

        int totalSec = isRest ? ses.restDuration : ses.focusDuration;
        if (totalSec <= 0) totalSec = 1;
        float prog = isIdle ? 0.0f : (float)(totalSec - remain) / (float)totalSec;
        if (prog > 1.0f) prog = 1.0f;
        Color arcCol = isRest ? P_GREEN : (isIdle ? P_IDLE : P_ACCENT);
        Pen arcPen(arcCol, 10.0f);
        arcPen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        float sweep = prog * 360.0f;
        if (sweep > 0.5f)
            g.DrawArc(&arcPen, cx-R, cy-R, R*2, R*2, -90.0f, sweep);

        Font* fBig = MkFont(32, FontStyleBold);
        Font* fSub = MkFont(13);
        SolidBrush arcTxBr(arcCol);
        std::wstring timeStr = isIdle ? L"准备开始" : FmtCountdown(remain);
        g.DrawString(timeStr.c_str(), -1, fBig,
            RectF((REAL)(cx-R), (REAL)(cy-22), (REAL)(R*2), 40), &sfC, &arcTxBr);
        std::wstring subStr = isIdle ? L"点击开始专注" :
            (isRest ? L"休息中..." :
             (L"第 " + std::to_wstring(ses.currentLoop+1) +
              L" / " + std::to_wstring(ses.loopCount) + L" 轮"));
        g.DrawString(subStr.c_str(), -1, fSub,
            RectF((REAL)(cx-R), (REAL)(cy+22), (REAL)(R*2), 22), &sfC, &subBr);
        delete fBig; delete fSub;

        // 循环点
        int dotY = cy + R + 18;
        int dotCount = std::min(ses.loopCount, 8);
        for (int i = 0; i < dotCount; i++) {
            int dotX = cx - (dotCount * 12) / 2 + i * 12 + 5;
            SolidBrush dotBr(i < ses.currentLoop ? arcCol : P_BORDER);
            g.FillEllipse(&dotBr, dotX-5, dotY-5, 10, 10);
        }
        y = (float)(cy + R + 38);
    }
    y += 12.0f;

    // ── 操作按钮行 ──────────────────────────────────────────
    {
        int bW = 100, bH = 42;
        int gap = 12;
        int totalW = isIdle ? (bW * 2 + gap) : (bW * 3 + gap * 2);
        int bx = (W - totalW) / 2;

        auto DrawBtn = [&](int x, int btnW, const wchar_t* label, Color bg, int hitId) {
            GraphicsPath bp;
            REAL r2 = 10.0f;
            bp.AddArc((REAL)x, (REAL)y, r2*2, r2*2, 180, 90);
            bp.AddArc((REAL)(x+btnW-r2*2), (REAL)y, r2*2, r2*2, 270, 90);
            bp.AddArc((REAL)(x+btnW-r2*2), (REAL)(y+bH-r2*2), r2*2, r2*2, 0, 90);
            bp.AddArc((REAL)x, (REAL)(y+bH-r2*2), r2*2, r2*2, 90, 90);
            bp.CloseFigure();
            SolidBrush btnBg(bg);
            g.FillPath(&btnBg, &bp);
            Font* fBtn = MkFont(14, FontStyleBold);
            g.DrawString(label, -1, fBtn,
                RectF((REAL)x, y, (REAL)btnW, (REAL)bH), &sfC, &wBr);
            delete fBtn;
            // HitZone 转换为屏幕坐标
            int screenY = (int)y - s_ScrollY;
            s_Hz.push_back({Gdiplus::Rect(x, screenY, btnW, bH), hitId});
        };

        if (isIdle) {
            DrawBtn(bx, bW*2+gap, L"开始专注", P_ACCENT, PH_BTN_START);
        } else if (!isRest) {
            DrawBtn(bx,              bW, L"停止",   P_BORDER, PH_BTN_STOP);
            DrawBtn(bx+bW+gap,       bW, L"完成",   P_GREEN,  PH_BTN_SKIP);
            DrawBtn(bx+(bW+gap)*2,   bW, L"跳过",   P_SUB,    PH_BTN_SKIP);
        } else {
            DrawBtn(bx, bW*2+gap, L"跳过休息", P_GREEN, PH_BTN_SKIP);
        }
        y += (REAL)bH + 18.0f;
    }

    // ── 设置行 ──────────────────────────────────────────────
    {
        Pen divPen(P_BORDER, 1.0f);
        g.DrawLine(&divPen, 16.0f, y, (REAL)(W-16), y);
        y += 12.0f;

        Font* fLabel = MkFont(12);
        Font* fVal   = MkFont(14, FontStyleBold);

        struct SettingItem { const wchar_t* label; std::wstring val; int hitDec; int hitInc; };
        SettingItem items[3] = {
            { L"专注时长", std::to_wstring(ses.focusDuration/60)+L" min", PH_FOCUS_DEC, PH_FOCUS_INC },
            { L"休息时长", std::to_wstring(ses.restDuration/60)+L" min",  PH_REST_DEC,  PH_REST_INC  },
            { L"循环次数", std::to_wstring(ses.loopCount)+L" 次",         PH_LOOP_DEC,  PH_LOOP_INC  },
        };

        float colW = (W - 32.0f) / 3.0f;
        for (int i = 0; i < 3; i++) {
            float cx2 = 16.0f + colW * i + colW / 2.0f;
            // label
            g.DrawString(items[i].label, -1, fLabel,
                RectF(16.0f + colW*i, y, colW, 18), &sfC, &subBr);
            // value
            SolidBrush valBr(isIdle ? P_TEXT : P_SUB);
            g.DrawString(items[i].val.c_str(), -1, fVal,
                RectF(16.0f + colW*i, y+20, colW, 24), &sfC, &valBr);
            // -/+ 按钮
            if (isIdle) {
                float btnSize = 24.0f;
                float btnY2 = y + 48.0f;
                float decX = cx2 - 44.0f, incX = cx2 + 20.0f;
                SolidBrush btnBg2(P_HOVER);
                Pen btnBorder(P_BORDER, 1.0f);
                // dec
                RectF decR(decX, btnY2, btnSize, btnSize);
                g.FillRectangle(&btnBg2, decR);
                g.DrawRectangle(&btnBorder, decR);
                g.DrawString(L"-", -1, fVal, decR, &sfC, &txBr);
                int sdY = (int)(btnY2) - s_ScrollY;
                s_Hz.push_back({Gdiplus::Rect((int)decX, sdY, (int)btnSize, (int)btnSize), items[i].hitDec});
                // inc
                RectF incR(incX, btnY2, btnSize, btnSize);
                g.FillRectangle(&btnBg2, incR);
                g.DrawRectangle(&btnBorder, incR);
                g.DrawString(L"+", -1, fVal, incR, &sfC, &txBr);
                int siY = (int)(btnY2) - s_ScrollY;
                s_Hz.push_back({Gdiplus::Rect((int)incX, siY, (int)btnSize, (int)btnSize), items[i].hitInc});
            }
        }
        delete fLabel; delete fVal;
        y += isIdle ? 82.0f : 52.0f;
    }

    // ── 绑定待办 ────────────────────────────────────────────
    {
        Pen divPen(P_BORDER, 1.0f);
        g.DrawLine(&divPen, 16.0f, y, (REAL)(W-16), y);
        y += 10.0f;

        Font* fH = MkFont(13, FontStyleBold);
        Font* fI = MkFont(13);
        SolidBrush acBr(P_ACCENT);
        g.DrawString(L"绑定待办", -1, fH, RectF(16, y, 120, 22), &sfL, &acBr);
        y += 26.0f;
        delete fH;

        // 🚀 每次渲染时重建 uuid 映射，确保 hitId 始终指向正确的 待办
        s_TodoHitUuid.clear();

        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        int todoCount = 0;
        for (const auto& t : g_Todos) {
            if (t.isDeleted || t.isDone) continue;
            if (todoCount >= 6) {
                g.DrawString(L"...", -1, fI, RectF(20, y, (REAL)(W-40), 22), &sfL, &subBr);
                y += 26.0f;
                break;
            }
            bool sel = (t.uuid == s_BoundTodo);
            if (sel) {
                SolidBrush selBg(Color(30, 239, 83, 80));
                g.FillRectangle(&selBg, 10, (int)y-2, W-20, 26);
            }
            if (sel) {
                SolidBrush dotBr(P_ACCENT);
                g.FillEllipse(&dotBr, 16, (int)y+8, 8, 8);
            }
            std::wstring disp = t.content.size() > 26 ? t.content.substr(0,24)+L".." : t.content;
            SolidBrush itemBr(sel ? P_ACCENT : P_TEXT);
            g.DrawString(disp.c_str(), -1, fI, RectF(30, y, (REAL)(W-46), 22), &sfL, &itemBr);
            int hitId = PH_TODO_BASE + todoCount;
            s_TodoHitUuid[hitId] = t.uuid;  // 🚀 记录 hitId → uuid
            int hY = (int)y - s_ScrollY;
            s_Hz.push_back({Gdiplus::Rect(10, hY, W-20, 26), hitId});
            todoCount++;
            y += 28.0f;
        }
        if (todoCount == 0) {
            g.DrawString(L"暂无活跃待办", -1, fI, RectF(20, y, (REAL)(W-40), 22), &sfL, &subBr);
            y += 26.0f;
        }
        delete fI;
        y += 6.0f;
    }

    // ── 标签区 ──────────────────────────────────────────────
    {
        Pen divPen(P_BORDER, 1.0f);
        g.DrawLine(&divPen, 16.0f, y, (REAL)(W-16), y);
        y += 10.0f;

        Font* fH = MkFont(13, FontStyleBold);
        Font* fT = MkFont(12);
        SolidBrush acBr(P_ACCENT);
        g.DrawString(L"标签", -1, fH, RectF(16, y, 80, 22), &sfL, &acBr);
        y += 26.0f;
        delete fH;

        float tx = 16.0f, ty = y;
        int tagIdx = 0;
        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        int visTagCount = 0;
        for (const auto& tag : g_PomodoroTags) {
            if (tag.isDeleted) continue;
            visTagCount++;
            bool sel = (std::find(s_SelTags.begin(), s_SelTags.end(), tag.uuid) != s_SelTags.end());
            std::wstring disp = tag.name.size() > 8 ? tag.name.substr(0,7)+L".." : tag.name;
            Font* fM = MkFont(12);
            RectF measured;
            g.MeasureString(disp.c_str(), -1, fM, PointF(0,0), &measured);
            delete fM;
            float tW = measured.Width + 20.0f;
            if (tW < 48) tW = 48;
            if (tx + tW > W - 16) { tx = 16.0f; ty += 30.0f; }

            GraphicsPath tp;
            REAL tr = 11.0f;
            tp.AddArc(tx,         ty,         tr*2, tr*2, 180, 90);
            tp.AddArc(tx+tW-tr*2, ty,         tr*2, tr*2, 270, 90);
            tp.AddArc(tx+tW-tr*2, ty+24-tr*2, tr*2, tr*2,   0, 90);
            tp.AddArc(tx,         ty+24-tr*2, tr*2, tr*2,  90, 90);
            tp.CloseFigure();

            unsigned int r=74, gv=108, b=247;
            if (tag.color.size() >= 7) {
                std::wstring hex = tag.color.substr(1);
                r  = (unsigned)wcstoul(hex.substr(0,2).c_str(), nullptr, 16);
                gv = (unsigned)wcstoul(hex.substr(2,2).c_str(), nullptr, 16);
                b  = (unsigned)wcstoul(hex.substr(4,2).c_str(), nullptr, 16);
            }
            Color tagCol(255,(BYTE)r,(BYTE)gv,(BYTE)b);
            SolidBrush tagBgBr(sel ? Color(180,(BYTE)r,(BYTE)gv,(BYTE)b)
                                   : Color(35,(BYTE)r,(BYTE)gv,(BYTE)b));
            g.FillPath(&tagBgBr, &tp);
            if (sel) {
                Pen tagBorder(tagCol, 1.5f);
                g.DrawPath(&tagBorder, &tp);
            }
            SolidBrush tagTxBr(sel ? Color(255,255,255,255) : tagCol);
            g.DrawString(disp.c_str(), -1, fT,
                RectF(tx+2, ty, tW-4, 24), &sfC, &tagTxBr);
            int hY = (int)ty - s_ScrollY;
            s_Hz.push_back({Gdiplus::Rect((int)tx, hY, (int)tW, 24), PH_TAG_BASE+tagIdx});
            tx += tW + 8.0f;
            tagIdx++;
        }
        delete fT;
        if (visTagCount == 0) {
            Font* fE = MkFont(12);
            g.DrawString(L"暂无标签（可在手机端添加）", -1, fE,
                RectF(16, ty, (REAL)(W-32), 22), &sfL, &subBr);
            delete fE;
            ty += 24.0f;
        }
        y = ty + 34.0f;
    }

    // ── 历史记录 ────────────────────────────────────────────
    {
        Pen divPen(P_BORDER, 1.0f);
        g.DrawLine(&divPen, 16.0f, y, (REAL)(W-16), y);
        y += 10.0f;

        Font* fH = MkFont(13, FontStyleBold);
        Font* fI = MkFont(12);
        SolidBrush acBr(P_ACCENT);
        g.DrawString(L"历史记录", -1, fH, RectF(16, y, 130, 22), &sfL, &acBr);

        // 🚀 "统计"按钮
        SolidBrush statsBr(P_IDLE);
        g.DrawString(L"统计", -1, fI, RectF((REAL)(W-130), y, 54, 22), &sfR, &statsBr);
        int hStatsY = (int)y - s_ScrollY;
        s_Hz.push_back({Gdiplus::Rect(W-136, hStatsY, 60, 22), PH_STATS_OPEN});

        SolidBrush refreshBr(P_IDLE);
        g.DrawString(L"刷新", -1, fI, RectF((REAL)(W-70), y, 54, 22), &sfR, &refreshBr);
        int hBtnY = (int)y - s_ScrollY;
        s_Hz.push_back({Gdiplus::Rect(W-70, hBtnY, 54, 22), PH_HIST_REFRESH});
        delete fH;
        y += 28.0f;

        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        if (g_PomodoroHistory.empty()) {
            g.DrawString(L"暂无专注记录", -1, fI, RectF(16, y, (REAL)(W-32), 22), &sfL, &subBr);
            y += 26.0f;
        } else {
            int total = (int)g_PomodoroHistory.size();
            int start = s_HistPage * HIST_PER_PAGE;
            int end   = (int)std::min(total, start + HIST_PER_PAGE);
            for (int i = start; i < end; i++) {
                const auto& rec = g_PomodoroHistory[i];
                time_t st = (time_t)(rec.startTime / 1000);
                struct tm tm_s; localtime_s(&tm_s, &st);
                wchar_t timeBuf[32];
                wcsftime(timeBuf, 32, L"%m-%d %H:%M", &tm_s);
                int minActual = rec.actualDuration / 60;
                bool ok = (rec.status == L"completed");
                Color rowCol = ok ? P_GREEN : P_SUB;
                SolidBrush rowBr(rowCol);
                // 行背景
                SolidBrush rowBg(ok ? Color(15, 52,199,120) : Color(10,130,140,160));
                g.FillRectangle(&rowBg, 10, (int)y-2, W-20, 24);
                std::wstring line = std::wstring(ok ? L"✓ " : L"✗ ") + timeBuf
                                  + L"  " + std::to_wstring(minActual) + L" min";
                g.DrawString(line.c_str(), -1, fI, RectF(16, y, (REAL)(W-120), 22), &sfL, &rowBr);

                // 🚀 右侧：优先用后端 JOIN 的 todoTitle，无则查本地 g_Todos
                std::wstring tdDisplay;
                if (!rec.todoTitle.empty()) {
                    tdDisplay = rec.todoTitle.size() > 14
                        ? rec.todoTitle.substr(0, 12) + L".." : rec.todoTitle;
                } else if (!rec.todoUuid.empty()) {
                    for (const auto& t : g_Todos) {
                        if (t.uuid == rec.todoUuid) {
                            tdDisplay = t.content.size() > 14
                                ? t.content.substr(0, 12) + L".." : t.content;
                            break;
                        }
                    }
                }
                if (!tdDisplay.empty())
                    g.DrawString(tdDisplay.c_str(), -1, fI,
                        RectF((REAL)(W-110), y, 94, 22), &sfR, &subBr);

                // 🚀 标签色块（最多显示3个）
                float tagX = 16.0f;
                int tagShown = 0;
                for (const auto& tagUuid : rec.tagUuids) {
                    if (tagShown >= 3) break;
                    for (const auto& tag : g_PomodoroTags) {
                        if (tag.uuid == tagUuid && !tag.isDeleted) {
                            // 解析颜色 "#RRGGBB"
                            Color tc(255, 99, 140, 220); // 默认蓝
                            if (tag.color.size() == 7 && tag.color[0] == L'#') {
                                unsigned int rv = 0, gv = 0, bv = 0;
                                swscanf_s(tag.color.c_str(), L"#%02x%02x%02x", &rv, &gv, &bv);
                                tc = Color(255, (BYTE)rv, (BYTE)gv, (BYTE)bv);
                            }
                            SolidBrush tagBr(tc);
                            g.FillEllipse(&tagBr, (int)tagX, (int)y + 8, 7, 7);
                            tagX += 10.0f;
                            tagShown++;
                            break;
                        }
                    }
                }

                y += 26.0f;
            }
            // 今日总计
            long long todayMs = 0;
            { time_t n=time(nullptr); struct tm tn; localtime_s(&tn,&n);
              tn.tm_hour=tn.tm_min=tn.tm_sec=0; todayMs=(long long)mktime(&tn)*1000LL; }
            int todayMins = 0;
            for (const auto& rec : g_PomodoroHistory)
                if (rec.startTime >= todayMs && rec.status == L"completed")
                    todayMins += rec.actualDuration / 60;
            y += 6.0f;
            std::wstring sumStr = L"今日已专注: " + std::to_wstring(todayMins) + L" min";
            SolidBrush sumBr(P_ACCENT);
            Font* fSum = MkFont(13, FontStyleBold);
            g.DrawString(sumStr.c_str(), -1, fSum, RectF(16, y, (REAL)(W-32), 22), &sfL, &sumBr);
            delete fSum;
            y += 26.0f;
        }
        delete fI;
    }

    // ── 🚀 远端专注感知横幅 ─────────────────────────────────
    {
        RemoteFocusState remote;
        {
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            remote = g_RemoteFocus;
        }
        if (remote.active && remote.targetEndMs > 0) {
            long long nowMs2   = (long long)time(nullptr) * 1000LL;
            long long diffMs   = remote.targetEndMs - nowMs2;
            int remainSec      = diffMs > 0 ? (int)(diffMs / 1000) : 0;

            // 超时超过30秒则自动清除
            if (diffMs < -30000) {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                g_RemoteFocus = RemoteFocusState{};
            } else {
                Color bannerBg = remote.isRestPhase ? P_GREEN : P_IDLE;
                // 横幅背景
                SolidBrush bannerBr(Color(220,
                    bannerBg.GetR(), bannerBg.GetG(), bannerBg.GetB()));
                GraphicsPath bp;
                REAL br2 = 10.0f;
                REAL bx  = 16.0f;
                REAL bw2 = (REAL)(W - 32);
                REAL bh  = 72.0f;
                bp.AddArc(bx,           y,        br2*2, br2*2, 180.0f,  90.0f);
                bp.AddArc(bx+bw2-br2*2, y,        br2*2, br2*2, 270.0f,  90.0f);
                bp.AddArc(bx+bw2-br2*2, y+bh-br2*2, br2*2, br2*2,   0.0f,  90.0f);
                bp.AddArc(bx,           y+bh-br2*2, br2*2, br2*2,  90.0f,  90.0f);
                bp.CloseFigure();
                g.FillPath(&bannerBr, &bp);

                Font* fBannerTitle = MkFont(11);
                Font* fBannerTime  = MkFont(20, FontStyleBold);
                SolidBrush wBr2(Color(255,255,255,255));
                SolidBrush wSubBr(Color(180,255,255,255));

                // 设备名
                std::wstring devLabel = remote.isRestPhase ? L"📱 休息中" : L"📱 专注中";
                // 截断设备ID显示
                std::wstring devShort = remote.sourceDevice.size() > 12
                    ? remote.sourceDevice.substr(0, 10) + L".."
                    : remote.sourceDevice;
                std::wstring titleStr = devLabel + L"  " + devShort;
                g.DrawString(titleStr.c_str(), -1, fBannerTitle,
                    RectF(28, y+6, (REAL)(W-56), 18), &sfL, &wSubBr);

                // 倒计时
                wchar_t tcBuf[16];
                swprintf_s(tcBuf, L"%02d:%02d", remainSec/60, remainSec%60);
                g.DrawString(tcBuf, -1, fBannerTime,
                    RectF(0, y+20, (REAL)W, 30), &sfC, &wBr2);

                // 待办内容
                if (!remote.todoContent.empty()) {
                    std::wstring tc = remote.todoContent.size() > 22
                        ? remote.todoContent.substr(0,20) + L".."
                        : remote.todoContent;
                    g.DrawString(tc.c_str(), -1, fBannerTitle,
                        RectF(28, y + bh - 20.0f, (REAL)(W-56), 16), &sfL, &wSubBr);
                }

                delete fBannerTitle; delete fBannerTime;
                y += bh + 8.0f;
            }
        }
    }

    y += 20.0f; // 底部留白
    s_ContentH = (int)y;

    // 约束滚动范围
    int maxScroll = std::max(0, s_ContentH - s_WinH);
    if (s_ScrollY > maxScroll) s_ScrollY = maxScroll;
    if (s_ScrollY < 0) s_ScrollY = 0;

    // ── 将画布裁剪到屏幕（应用滚动）────────────────────────
    // 先把标题行（固定区，约62px）从画布直接拷到屏幕
    const int FIXED_H = 62;
    // 画布 y=0..FIXED_H 对应屏幕 y=0..FIXED_H（不偏移）
    BitBlt(hdcWin, 0, 0, W, FIXED_H, hdcCanvas, 0, 0, SRCCOPY);
    // 画布 y=FIXED_H+scrollY 对应屏幕 y=FIXED_H（偏移滚动）
    int srcY = FIXED_H + s_ScrollY;
    int copyH = H - FIXED_H;
    if (copyH > 0)
        BitBlt(hdcWin, 0, FIXED_H, W, copyH, hdcCanvas, 0, srcY, SRCCOPY);

    // ── 绘制滚动条 ──────────────────────────────────────────
    if (s_ContentH > s_WinH) {
        int sbW = 5, sbX = W - sbW - 2;
        float ratio  = (float)(s_WinH) / (float)s_ContentH;
        int   sbH    = std::max(30, (int)(H * ratio));
        int   sbMaxY = H - sbH;
        int   sbY    = maxScroll > 0 ? (int)((float)sbMaxY * s_ScrollY / maxScroll) : 0;
        HBRUSH sbBrush = CreateSolidBrush(RGB(180, 190, 210));
        RECT sbRc = {sbX, sbY, sbX+sbW, sbY+sbH};
        FillRect(hdcWin, &sbRc, sbBrush);
        DeleteObject(sbBrush);
    }

    SelectObject(hdcCanvas, hCanvasOld);
    DeleteObject(hCanvasBmp);
    DeleteDC(hdcCanvas);
    ReleaseDC(s_hWnd, hdcWin);
}

// ============================================================
// 定时器回调：计时到期处理
// ============================================================
static void HandleTimerTick() {
    // Guard against WM_TIMER re-entry during modal MessageBox loops.
    static bool s_InTickHandler = false;
    if (s_InTickHandler) return;
    s_InTickHandler = true;

    auto& s = g_PomodoroSession;
    do {
        if (s.status == PomodoroStatus::Idle) break;

        long long nowMs = (long long)time(nullptr) * 1000LL;
        if (nowMs < s.targetEndMs) {
            PomodoroRender();
            break;
        }

        // 时间到
        if (!s.isRestPhase) {
            FinishCurrentRecord(L"completed");
            s.currentLoop++;
            if (s.currentLoop >= s.loopCount) {
                // Commit final idle state first to avoid re-entrant duplicate popups.
                s.status      = PomodoroStatus::Idle;
                s.currentLoop = 0;
                s.isRestPhase = false;
                s.targetEndMs = 0;
                WsPomodoroSendStop();
                SavePomodoroSession();
                PomodoroRender();
                MessageBoxW(s_hWnd, L"所有专注循环完成！好好休息一下吧。", L"番茄钟", MB_ICONINFORMATION);
            } else {
                // Commit rest state first to avoid duplicate "进入休息" dialogs.
                s.isRestPhase = true;
                s.status      = PomodoroStatus::Resting;
                s.targetEndMs = nowMs + (long long)s.restDuration * 1000LL;
                WsPomodoroSendStart(s.targetEndMs, s.restDuration, L"", L"", true);
                SavePomodoroSession();
                PomodoroRender();
                MessageBoxW(s_hWnd, L"专注结束！进入休息时间。", L"番茄钟", MB_ICONINFORMATION);
            }
        } else {
            // 休息 -> 下一轮专注：先切状态，再弹窗。
            s.isRestPhase       = false;
            s.status            = PomodoroStatus::Focusing;
            s.currentRecordUuid = GenUuid();
            s.targetEndMs       = nowMs + (long long)s.focusDuration * 1000LL;

            std::wstring todoContent;
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                for (const auto& t : g_Todos)
                    if (t.uuid == s.boundTodoUuid) { todoContent = t.content; break; }
            }
            WsPomodoroSendStart(s.targetEndMs, s.focusDuration, todoContent, s.boundTodoUuid, false);
            SavePomodoroSession();
            PomodoroRender();
            MessageBoxW(s_hWnd, L"休息结束！开始下一轮专注。", L"番茄钟", MB_ICONINFORMATION);
        }

        // Keep UI/state consistent even if no modal message was shown.
        SavePomodoroSession();
        PomodoroRender();
    } while (false);

    s_InTickHandler = false;
}

// ============================================================
// 事件处理：点击
// ============================================================
static void HandleHit(int hitId) {
    auto& s = g_PomodoroSession;
    long long nowMs = (long long)time(nullptr) * 1000LL;

    if (hitId == PH_CLOSE) {
        if (s.status == PomodoroStatus::Focusing) {
            if (MessageBoxW(s_hWnd, L"专注进行中，确定要关闭？",
                            L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
            FinishCurrentRecord(L"interrupted");
            WsPomodoroSendStop(); // 🚀 WS 广播：关闭中断
            s.status = PomodoroStatus::Idle;
            s.targetEndMs = 0;
            SavePomodoroSession();
        }
        DestroyWindow(s_hWnd);
        return;
    }

    if (hitId == PH_BTN_START) {
        s.status            = PomodoroStatus::Focusing;
        s.isRestPhase       = false;
        s.currentLoop       = 0;
        s.currentRecordUuid = GenUuid();
        s.boundTodoUuid     = s_BoundTodo;
        s.targetEndMs       = nowMs + (long long)s.focusDuration * 1000LL;
        SavePomodoroSession();
        // 🚀 WS 广播：本机开始专注
        {
            std::wstring todoContent;
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            for (const auto& t : g_Todos)
                if (t.uuid == s_BoundTodo) { todoContent = t.content; break; }
            WsPomodoroSendStart(s.targetEndMs, s.focusDuration, todoContent, s_BoundTodo, false);
        }
    }
    else if (hitId == PH_BTN_STOP) {
        if (MessageBoxW(s_hWnd, L"确定要中止专注？", L"确认", MB_YESNO) == IDYES) {
            FinishCurrentRecord(L"interrupted");
            WsPomodoroSendStop(); // 🚀 WS 广播：中断
            s.status      = PomodoroStatus::Idle;
            s.isRestPhase = false;
            s.targetEndMs = 0;
            s.currentLoop = 0;
            SavePomodoroSession();
        }
    }
    else if (hitId == PH_BTN_SKIP) {
        if (!s.isRestPhase) {
            FinishCurrentRecord(L"completed");
            s.currentLoop++;
            if (s.currentLoop >= s.loopCount) {
                s.status      = PomodoroStatus::Idle;
                s.currentLoop = 0;
                s.targetEndMs = 0;
                WsPomodoroSendStop(); // 🚀 所有循环完成
            } else {
                s.isRestPhase = true;
                s.status      = PomodoroStatus::Resting;
                s.targetEndMs = nowMs + (long long)s.restDuration * 1000LL;
                // 🚀 通知其他端进入休息阶段
                WsPomodoroSendStart(s.targetEndMs, s.restDuration, L"", L"", true);
            }
        } else {
            s.isRestPhase       = false;
            s.status            = PomodoroStatus::Focusing;
            s.currentRecordUuid = GenUuid();
            s.targetEndMs       = nowMs + (long long)s.focusDuration * 1000LL;
            // 🚀 通知其他端跳过休息，继续专注
            {
                std::wstring todoContent;
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                for (const auto& t : g_Todos)
                    if (t.uuid == s.boundTodoUuid) { todoContent = t.content; break; }
                WsPomodoroSendStart(s.targetEndMs, s.focusDuration, todoContent, s.boundTodoUuid, false);
            }
        }
        SavePomodoroSession();
    }
    else if (hitId == PH_FOCUS_DEC) { if (s.focusDuration > 5*60)  s.focusDuration -= 5*60;  SavePomodoroSession(); }
    else if (hitId == PH_FOCUS_INC) { if (s.focusDuration < 90*60) s.focusDuration += 5*60;  SavePomodoroSession(); }
    else if (hitId == PH_REST_DEC)  { if (s.restDuration > 1*60)   s.restDuration  -= 1*60;  SavePomodoroSession(); }
    else if (hitId == PH_REST_INC)  { if (s.restDuration < 30*60)  s.restDuration  += 1*60;  SavePomodoroSession(); }
    else if (hitId == PH_LOOP_DEC)  { if (s.loopCount > 1)         s.loopCount--;             SavePomodoroSession(); }
    else if (hitId == PH_LOOP_INC)  { if (s.loopCount < 12)        s.loopCount++;             SavePomodoroSession(); }
    else if (hitId >= PH_TAG_BASE && hitId < PH_TODO_BASE) {
        int idx = hitId - PH_TAG_BASE;
        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        int cur = 0;
        for (const auto& tag : g_PomodoroTags) {
            if (tag.isDeleted) continue;
            if (cur == idx) {
                auto it = std::find(s_SelTags.begin(), s_SelTags.end(), tag.uuid);
                if (it != s_SelTags.end()) s_SelTags.erase(it);
                else s_SelTags.push_back(tag.uuid);
                break;
            }
            cur++;
        }
    }
    else if (hitId >= PH_TODO_BASE && hitId < PH_HIST_REFRESH) {
        // 🚀 直接用渲染时建立的 hitId→uuid 映射，不依赖运行时 g_Todos 顺序
        auto it = s_TodoHitUuid.find(hitId);
        if (it != s_TodoHitUuid.end()) {
            s_BoundTodo = (s_BoundTodo == it->second) ? L"" : it->second;
        }
    }
    else if (hitId == PH_HIST_REFRESH) {
        long long toMs   = nowMs;
        long long fromMs = toMs - (long long)30 * 24 * 3600 * 1000LL;
        std::thread([fromMs, toMs]() {
            ApiFetchPomodoroHistory(fromMs, toMs);
            SavePomodoroLocalCache();
            if (s_hWnd) InvalidateRect(s_hWnd, NULL, FALSE);
        }).detach();
    }
    else if (hitId == PH_STATS_OPEN) {
        ShowPomodoroStatsWindow(s_hWnd);
        return; // 不需要重绘
    }

    PomodoroRender();
}

// ============================================================
// WndProc
// ============================================================
static LRESULT CALLBACK PomodoroWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        s_hWnd = hWnd;
        SetTimer(hWnd, TIMER_ID_TICK, 500, NULL);
        // 🚀 WsPomodoroConnect() 在 main.cpp 登录成功后已调用，此处不重复
        std::thread([]() {
            // 先从本地缓存恢复历史
            LoadPomodoroLocalCache();
            // 补传断网时未上传的记录
            UploadPendingPomodoroRecords();
            // 拉取标签
            ApiFetchPomodoroTags();
            // 拉取最近 30 天云端记录（合并到本地）
            long long toMs   = (long long)time(nullptr) * 1000LL;
            long long fromMs = toMs - (long long)30 * 24 * 3600 * 1000LL;
            ApiFetchPomodoroHistory(fromMs, toMs);
            // 云端数据写回本地缓存
            SavePomodoroLocalCache();
            if (s_hWnd) InvalidateRect(s_hWnd, NULL, FALSE);
        }).detach();
        break;

    case WM_TIMER:
        if (wp == TIMER_ID_TICK) HandleTimerTick();
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hWnd, &ps); EndPaint(hWnd, &ps);
        PomodoroRender();
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        s_ScrollY -= delta / 3;
        if (s_ScrollY < 0) s_ScrollY = 0;
        int maxS = std::max(0, s_ContentH - s_WinH);
        if (s_ScrollY > maxS) s_ScrollY = maxS;
        PomodoroRender();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp), y = HIWORD(lp);
        for (const auto& hz : s_Hz) {
            if (hz.rect.Contains(x, y)) {
                HandleHit(hz.id);
                return 0;
            }
        }
        // 没命中任何按钮则允许拖动窗口
        SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        break;
    }

    case WM_SIZE:
        s_WinH = HIWORD(lp);
        PomodoroRender();
        break;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID_TICK);
        // 🚀 不在此处断开 WebSocket：主页横幅需要持续感知跨端状态
        //    WebSocket 的生命期由 main.cpp 统一管理（退出登录/程序退出时断开）
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
void ShowPomodoroWindow(HWND parent) {
    if (s_hWnd && IsWindow(s_hWnd)) {
        SetForegroundWindow(s_hWnd);
        return;
    }

    static bool s_Registered = false;
    if (!s_Registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = PomodoroWndProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"PomodoroWnd";
        RegisterClassExW(&wc);
        s_Registered = true;
    }

    s_hParent  = parent;
    s_ScrollY  = 0;

    LoadPomodoroSession();

    int W = 420, H = 800;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int wx = (sx - W) / 2, wy = (sy - H) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"PomodoroWnd", L"番茄钟",
        WS_OVERLAPPEDWINDOW,
        wx, wy, W, H,
        parent, NULL, GetModuleHandleW(NULL), NULL);

    if (!hWnd) return;
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
}

