/**
 * pomodoro_overlay.cpp
 */
#include "pomodoro_overlay.h"
#include "common.h"
#include "utils.h"
#include <string>
#include <vector>
#include <sstream>
#include <gdiplus.h>
using namespace Gdiplus;

// Ensure macros are defined before usage
#define OV_W 300 // Default width of the overlay
#define OV_H 200 // Default height of the overlay

// ============================================================
// 全局持久化配置
// ============================================================
BYTE g_OverlayAlpha = 200;
int  g_OverlayX     = -1;
int  g_OverlayY     = -1;

// Added configurable font size and window size
static int g_OverlayFontSize = 32;
static int g_OverlayWidth = OV_W;
static int g_OverlayHeight = OV_H;

// ============================================================
// 内部状态
// ============================================================
static HWND        s_hWnd      = NULL;
static OverlayInfo s_Info;
static bool        s_Dragging  = false;
static POINT       s_DragStart = {};
static POINT       s_WinStart  = {};

// 跨端状态的本地快照（仅主线程读写，WS线程通过PostMessage通知刷新）
static RemoteFocusState s_RemoteSnapshot;

static const UINT_PTR OV_TIMER   = 4001;
static const UINT     WM_OV_SYNC = WM_USER + 201;  // WS线程 PostMessage 用

// ============================================================
// 工具
// ============================================================
static std::wstring FmtSecs(int secs) {
    if (secs < 0) secs = 0;
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", secs / 60, secs % 60);
    return buf;
}

static std::wstring JoinTags(const std::vector<std::wstring>& tags) {
    std::wstring out;
    try {
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i) out += L" ";
            out += L"#" + tags[i];
        }
    } catch (const std::exception& e) {
        // Log or handle the error
        out = L""; // Reset to a safe state
    }
    return out;
}

// ============================================================
// 绘制（只在主线程调用，GDI 安全）
// ============================================================
static void OvRender() {
    if (!s_hWnd) return;
    if (!g_MiSansFamily) return;

    RECT rc;
    GetClientRect(s_hWnd, &rc);
    int W = g_OverlayWidth, H = g_OverlayHeight;
    if (W <= 0 || H <= 0) return;

    // ── 创建 32-bit DIBSection（支持逐像素 Alpha）──
    HDC hdcScreen = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = W;
    bmi.bmiHeader.biHeight      = -H;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP memBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!memBmp) {
        DeleteDC(memDC);
        ReleaseDC(NULL, hdcScreen);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // 清零（全透明）
    memset(pBits, 0, W * H * 4);

    // ── GDI+ 绘制 ──
    {
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        // 背景圆角矩形
        Color bgColor = s_Info.isRest
            ? Color(210, 10, 40, 30)
            : Color(210, 15, 20, 30);
        SolidBrush bgBrush(bgColor);

        GraphicsPath path;
        int r = 12;
        path.AddArc((INT)0,       (INT)0,       (INT)(2*r), (INT)(2*r), 180.0f, 90.0f);
        path.AddArc((INT)(W-2*r), (INT)0,       (INT)(2*r), (INT)(2*r), 270.0f, 90.0f);
        path.AddArc((INT)(W-2*r), (INT)(H-2*r), (INT)(2*r), (INT)(2*r),   0.0f, 90.0f);
        path.AddArc((INT)0,       (INT)(H-2*r), (INT)(2*r), (INT)(2*r),  90.0f, 90.0f);
        path.CloseFigure();
        g.FillPath(&bgBrush, &path);

        // 边框
        Color borderCol = s_Info.isRest
            ? Color(180, 52, 199, 120)
            : Color(180, 239, 83, 80);
        Pen borderPen(borderCol, 1.5f);
        g.DrawPath(&borderPen, &path);

        // ── 行1：倒计时 ──
        std::wstring countdown = s_Info.active ? FmtSecs(s_Info.remainSecs) : L"--:--";
        Font fBig(g_MiSansFamily, (REAL)g_OverlayFontSize, FontStyleBold, UnitPixel);
        SolidBrush whiteBr(Color(255, 255, 255, 255));
        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        RectF rcCD((REAL)0, (REAL)8, (REAL)W, (REAL)42);
        g.DrawString(countdown.c_str(), -1, &fBig, rcCD, &sfCenter, &whiteBr);

        // ── 行2：阶段 + 轮次 ──
        std::wstring phase = s_Info.isRest ? L"休息中" : L"专注中";
        if (!s_Info.active) phase = L"闲置";
        if (s_Info.isRemote) phase += L" (跨端)";
        std::wstring line2 = phase;
        if (s_Info.active && s_Info.totalLoops > 0) {
            wchar_t lb[32];
            swprintf_s(lb, L"  %d/%d 轮", s_Info.currentLoop, s_Info.totalLoops);
            line2 += lb;
        }
        Font fSub(g_MiSansFamily, 11.0f, FontStyleRegular, UnitPixel);
        Color subCol = s_Info.isRest ? Color(200,100,240,160) : Color(200,255,150,130);
        SolidBrush subBr(subCol);
        StringFormat sfLeft;
        sfLeft.SetAlignment(StringAlignmentNear);
        sfLeft.SetTrimming(StringTrimmingEllipsisCharacter);
        sfLeft.SetFormatFlags(StringFormatFlagsNoWrap);
        RectF rcPhase((REAL)8, (REAL)52, (REAL)(W-16), (REAL)18);
        g.DrawString(line2.c_str(), -1, &fSub, rcPhase, &sfLeft, &subBr);

        // ── 行3：待办内容 ──
        if (!s_Info.todoContent.empty()) {
            SolidBrush todoBr(Color(230,230,240,255));
            RectF rcTodo((REAL)8, (REAL)72, (REAL)(W-16), (REAL)18);
            std::wstring todoDisp = L"  " + s_Info.todoContent;
            g.DrawString(todoDisp.c_str(), -1, &fSub, rcTodo, &sfLeft, &todoBr);
        }

        // ── 行4：标签 ──
        if (!s_Info.tagNames.empty()) {
            std::wstring tagsStr = JoinTags(s_Info.tagNames);
            Font fTag(g_MiSansFamily, 10.0f, FontStyleRegular, UnitPixel);
            SolidBrush tagBr(Color(180,160,200,255));
            RectF rcTag((REAL)8, (REAL)92, (REAL)(W-16), (REAL)18);
            g.DrawString(tagsStr.c_str(), -1, &fTag, rcTag, &sfLeft, &tagBr);
        }

        // ── 关闭按钮 ──
        Font fX(g_MiSansFamily, 10.0f, FontStyleBold, UnitPixel);
        SolidBrush xBr(Color(150,200,200,200));
        RectF rcX((REAL)(W-20), (REAL)4, (REAL)16, (REAL)14);
        StringFormat sfXFmt;
        sfXFmt.SetAlignment(StringAlignmentCenter);
        sfXFmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"x", -1, &fX, rcX, &sfXFmt, &xBr);
    }

    // ── UpdateLayeredWindow ──
    BLENDFUNCTION bf = {};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;          // 整体透明度由逐像素Alpha控制
    bf.AlphaFormat         = AC_SRC_ALPHA;

    RECT wrect;
    GetWindowRect(s_hWnd, &wrect);
    POINT ptDest = {wrect.left, wrect.top};
    POINT ptSrc  = {0, 0};
    SIZE  szWnd  = {W, H};

    UpdateLayeredWindow(s_hWnd, hdcScreen, &ptDest, &szWnd,
                        memDC, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    ReleaseDC(NULL, hdcScreen);
}

// ============================================================
// WndProc
// ============================================================
static LRESULT CALLBACK OvWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hWnd, OV_TIMER, 500, NULL);
        break;

    case WM_TIMER:
        if (wp == OV_TIMER) {
            long long nowMs = (long long)time(nullptr) * 1000LL;
            auto& ps = g_PomodoroSession;

            if (ps.status != PomodoroStatus::Idle) {
                // 本地专注（主线程数据，无竞争）
                long long diff = ps.targetEndMs - nowMs;
                s_Info.remainSecs  = (diff > 0) ? (int)(diff / 1000) : 0;
                s_Info.active      = true;
                s_Info.isRest      = ps.isRestPhase;
                s_Info.currentLoop = ps.currentLoop + 1;
                s_Info.totalLoops  = ps.loopCount;
                s_Info.isRemote    = false;
                // todoContent/tagNames 由 UpdatePomodoroOverlay 写入
            } else {
                // 跨端专注：使用 WM_OV_SYNC 同步的快照，完全在主线程，无锁竞争
                if (s_RemoteSnapshot.active && s_RemoteSnapshot.targetEndMs > 0) {
                    long long diff = s_RemoteSnapshot.targetEndMs - nowMs;
                    if (diff < -30000LL) {
                        // 超时自动清除
                        s_RemoteSnapshot = RemoteFocusState{};
                        s_Info.active = false;
                    } else {
                        s_Info.remainSecs  = (diff > 0) ? (int)(diff / 1000) : 0;
                        s_Info.active      = true;
                        s_Info.isRest      = s_RemoteSnapshot.isRestPhase;
                        s_Info.currentLoop = 0;
                        s_Info.totalLoops  = 0;
                        s_Info.isRemote    = true;
                        s_Info.todoContent = s_RemoteSnapshot.todoContent;
                        s_Info.tagNames    = s_RemoteSnapshot.tagNames;
                    }
                } else {
                    s_Info.active = false;
                }
            }
            OvRender();
        }
        break;

    case WM_OV_SYNC: {
        RemoteFocusState* pRf = reinterpret_cast<RemoteFocusState*>(lp);
        if (pRf) {
            s_RemoteSnapshot = *pRf;
            delete pRf; // Properly delete dynamically allocated memory
        }
        if (s_RemoteSnapshot.active) {
            if (s_hWnd && IsWindow(s_hWnd) && !IsWindowVisible(s_hWnd))
                ShowWindow(s_hWnd, SW_SHOWNOACTIVATE);
        } else {
            if (g_PomodoroSession.status == PomodoroStatus::Idle)
                if (s_hWnd && IsWindow(s_hWnd))
                    ShowWindow(s_hWnd, SW_HIDE);
        }
        OvRender();
        break;
    }

    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int W = rc.right;
        POINT pt = {LOWORD(lp), HIWORD(lp)};
        if (pt.x >= W - 20 && pt.y <= 18) {
            HidePomodoroOverlay();
            return 0;
        }
        s_Dragging = true;
        s_DragStart = pt;
        RECT wr;
        GetWindowRect(hWnd, &wr);
        s_WinStart.x = wr.left;
        s_WinStart.y = wr.top;
        SetCapture(hWnd);
        break;
    }
    case WM_MOUSEMOVE:
        if (s_Dragging) {
            POINT cur = {LOWORD(lp), HIWORD(lp)};
            int nx = s_WinStart.x + (cur.x - s_DragStart.x);
            int ny = s_WinStart.y + (cur.y - s_DragStart.y);
            SetWindowPos(hWnd, NULL, nx, ny, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            g_OverlayX = nx;
            g_OverlayY = ny;
            OvRender();
        }
        break;
    case WM_LBUTTONUP:
        if (s_Dragging) {
            s_Dragging = false;
            ReleaseCapture();
            SaveOverlaySettings();
        }
        break;

    case WM_USER_REFRESH:
        OvRender();
        break;

    case WM_DESTROY:
        KillTimer(hWnd, OV_TIMER);
        if (s_hWnd) {
            DestroyWindow(s_hWnd);
            s_hWnd = NULL;
        }
        break;

    default:
        return DefWindowProcW(hWnd, msg, wp, lp);
    }
    return 0;
}

// ============================================================
// 供 WS 接收线程调用：安全地把 RemoteFocusState 推送给 Overlay 主线程
// ============================================================
void NotifyOverlayRemoteFocus(const RemoteFocusState& rf) {
    if (!s_hWnd || !IsWindow(s_hWnd)) return;
    RemoteFocusState* p = new RemoteFocusState(rf);
    PostMessageW(s_hWnd, WM_OV_SYNC, 0, reinterpret_cast<LPARAM>(p));
}

void NotifyOverlayRemoteStop() {
    if (!s_hWnd || !IsWindow(s_hWnd)) return;
    RemoteFocusState* p = new RemoteFocusState{};  // active=false
    PostMessageW(s_hWnd, WM_OV_SYNC, 0, reinterpret_cast<LPARAM>(p));
}

// ============================================================
// 公开接口
// ============================================================

// 内部：注册窗口类 + 创建隐藏窗口（只执行一次）
static void EnsureOverlayCreated() {
    if (s_hWnd && IsWindow(s_hWnd)) return;

    static bool s_Reg = false;
    if (!s_Reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = OvWndProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hbrBackground = NULL;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"PomodoroOverlay";
        RegisterClassExW(&wc);
        s_Reg = true;
    }

    LoadOverlaySettings();

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int W = (int)(OV_W * g_Scale);
    int H = (int)(OV_H * g_Scale);
    int X = (g_OverlayX >= 0) ? g_OverlayX : (scrW - W - 20);
    int Y = (g_OverlayY >= 0) ? g_OverlayY : 20;

    s_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"PomodoroOverlay", L"",
        WS_POPUP,
        X, Y, W, H,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    // 创建后保持隐藏，等待专注状态触发显示
}

// 程序启动时调用（主线程）：预创建隐藏窗口，确保 s_hWnd 有效
// 这样 WS 线程收到跨端消息时 PostMessageW 才能找到窗口
void InitPomodoroOverlay() {
    EnsureOverlayCreated();
}

void ShowPomodoroOverlay() {
    EnsureOverlayCreated();
    if (!s_hWnd) return;
    ShowWindow(s_hWnd, SW_SHOWNOACTIVATE);
    OvRender();
}

void HidePomodoroOverlay() {
    if (s_hWnd && IsWindow(s_hWnd))
        ShowWindow(s_hWnd, SW_HIDE);
}

void UpdatePomodoroOverlay(const OverlayInfo& info) {
    s_Info = info;
    if (s_hWnd && IsWindow(s_hWnd) && IsWindowVisible(s_hWnd))
        OvRender();
}

// ============================================================
// 持久化
// ============================================================
void SaveOverlaySettings() {
    std::wstring path = SETTINGS_FILE;
    WritePrivateProfileStringW(L"Overlay", L"alpha",
        std::to_wstring(g_OverlayAlpha).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Overlay", L"x",
        std::to_wstring(g_OverlayX).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Overlay", L"y",
        std::to_wstring(g_OverlayY).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Overlay", L"fontSize",
        std::to_wstring(g_OverlayFontSize).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Overlay", L"width",
        std::to_wstring(g_OverlayWidth).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Overlay", L"height",
        std::to_wstring(g_OverlayHeight).c_str(), path.c_str());
}

void LoadOverlaySettings() {
    std::wstring path = SETTINGS_FILE;
    g_OverlayAlpha = (BYTE)GetPrivateProfileIntW(L"Overlay", L"alpha", 200, path.c_str());
    g_OverlayX     = GetPrivateProfileIntW(L"Overlay", L"x", -1, path.c_str());
    g_OverlayY     = GetPrivateProfileIntW(L"Overlay", L"y", -1, path.c_str());
    g_OverlayFontSize = GetPrivateProfileIntW(L"Overlay", L"fontSize", 32, path.c_str());
    g_OverlayWidth = GetPrivateProfileIntW(L"Overlay", L"width", OV_W, path.c_str());
    g_OverlayHeight = GetPrivateProfileIntW(L"Overlay", L"height", OV_H, path.c_str());
}
