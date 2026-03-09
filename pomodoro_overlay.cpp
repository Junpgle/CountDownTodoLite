/**
 * pomodoro_overlay.cpp
 */
#include "pomodoro_overlay.h"
#include "common.h"
#include "utils.h"
#include <shlwapi.h>
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
int g_OverlayWidth = OV_W;
int g_OverlayHeight = OV_H;
static const int RESIZE_BORDER = 6; // 像素，与 g_Scale 无关

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


enum ResizeDir { RD_NONE, RD_RIGHT, RD_BOTTOM, RD_CORNER };

static ResizeDir GetResizeDir(HWND hWnd, POINT pt) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;
    int b = RESIZE_BORDER;
    bool onRight  = (pt.x >= W - b && pt.x <= W);
    bool onBottom = (pt.y >= H - b && pt.y <= H);
    if (onRight && onBottom) return RD_CORNER;
    if (onRight)  return RD_RIGHT;
    if (onBottom) return RD_BOTTOM;
    return RD_NONE;
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

    int W = (int)(g_OverlayWidth  * g_Scale);
    int H = (int)(g_OverlayHeight * g_Scale);
    if (W <= 0 || H <= 0) return;

    // 缩放后的常用尺寸
    float sc = g_Scale;
    int r = (int)(12 * sc);  // 圆角半径

    HDC hdcScreen = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = W;
    bmi.bmiHeader.biHeight      = -H;
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
    memset(pBits, 0, W * H * 4);

    {
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        float sc = (float)g_Scale;
        int r = (int)(12 * sc);

        // ── 背景 + 边框 ──
        Color bgColor = s_Info.isRest
            ? Color(210, 10, 40, 30)
            : Color(210, 15, 20, 30);
        SolidBrush bgBrush(bgColor);
        GraphicsPath path;
        path.AddArc(0,       0,       2*r, 2*r, 180.0f, 90.0f);
        path.AddArc(W-2*r,   0,       2*r, 2*r, 270.0f, 90.0f);
        path.AddArc(W-2*r,   H-2*r,   2*r, 2*r,   0.0f, 90.0f);
        path.AddArc(0,       H-2*r,   2*r, 2*r,  90.0f, 90.0f);
        path.CloseFigure();
        g.FillPath(&bgBrush, &path);

        Color borderCol = s_Info.isRest
            ? Color(180, 52, 199, 120)
            : Color(180, 239, 83, 80);
        Pen borderPen(borderCol, 1.5f * sc);
        g.DrawPath(&borderPen, &path);

        // ── 上下分割线 ──
        int divY = (int)(H * 2.0f / 3.0f);
        Color divCol(80, 200, 200, 200);
        Pen divPen(divCol, 1.0f * sc);
        g.DrawLine(&divPen, (int)(8*sc), divY, W - (int)(8*sc), divY);

        // ── 下半左右分割线 ──
        Color divColV(60, 200, 200, 200);
        Pen divPenV(divColV, 1.0f * sc);
        g.DrawLine(&divPenV, W/2, divY + (int)(4*sc), W/2, H - (int)(4*sc));

        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        sfCenter.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfLeft;
        sfLeft.SetAlignment(StringAlignmentNear);
        sfLeft.SetLineAlignment(StringAlignmentCenter);
        sfLeft.SetTrimming(StringTrimmingEllipsisCharacter);
        sfLeft.SetFormatFlags(StringFormatFlagsNoWrap);

        SolidBrush whiteBr(Color(255, 255, 255, 255));

        // ══════════════════════════════════════
        // 上2/3区域：倒计时 + 阶段副标题
        // ══════════════════════════════════════

        // 倒计时：占上2/3的上部约60%
        std::wstring countdown = s_Info.active ? FmtSecs(s_Info.remainSecs) : L"--:--";
        // 字号：上2/3高度的约50%
        float cdFontSize = divY * 0.50f;
        Font fBig(g_MiSansFamily, cdFontSize, FontStyleBold, UnitPixel);
        // 关闭按钮占右上角，倒计时区域留出右侧空间
        RectF rcCD((REAL)(8*sc), (REAL)(6*sc), (REAL)(W - 16*sc), (REAL)(divY * 0.65f));
        g.DrawString(countdown.c_str(), -1, &fBig, rcCD, &sfCenter, &whiteBr);

        // 阶段 + 轮次：上2/3的下部
        std::wstring phase = s_Info.isRest ? L"休息中" : L"专注中";
        if (!s_Info.active) phase = L"闲置";
        if (s_Info.isRemote) phase += L" (跨端)";
        std::wstring line2 = phase;
        if (s_Info.active && s_Info.totalLoops > 0) {
            wchar_t lb[32];
            swprintf_s(lb, L"  %d/%d 轮", s_Info.currentLoop, s_Info.totalLoops);
            line2 += lb;
        }
        float subFontSize = divY * 0.14f;
        if (subFontSize < 11*sc) subFontSize = 11*sc;
        Font fSub(g_MiSansFamily, subFontSize, FontStyleRegular, UnitPixel);
        Color subCol = s_Info.isRest ? Color(200,100,240,160) : Color(200,255,150,130);
        SolidBrush subBr(subCol);
        float subH = subFontSize * 1.6f;
        RectF rcPhase((REAL)(8*sc), (REAL)(divY * 0.65f), (REAL)(W - 16*sc), subH);
        g.DrawString(line2.c_str(), -1, &fSub, rcPhase, &sfCenter, &subBr);

        // ══════════════════════════════════════
        // 下1/3区域
        // ══════════════════════════════════════
        float botH    = (float)(H - divY);          // 下1/3高度
        float botMidY = divY + botH * 0.5f;         // 下区垂直中心
        float botFontSize = botH * 0.30f;
        if (botFontSize < 10*sc) botFontSize = 10*sc;
        Font fBot(g_MiSansFamily, botFontSize, FontStyleRegular, UnitPixel);
        float botLineH = botFontSize * 1.5f;

        float padX = 8*sc;
        float halfW = W * 0.5f;

        // 左侧：待办任务（带图标前缀）
        {
            std::wstring todoText = s_Info.todoContent.empty()
                ? L"无任务"
                : (L"▸ " + s_Info.todoContent);
            Color todoCol = s_Info.todoContent.empty()
                ? Color(120, 180, 180, 180)
                : Color(230, 230, 240, 255);
            SolidBrush todoBr(todoCol);
            RectF rcTodo(padX,
                         botMidY - botLineH * 0.5f,
                         halfW - padX * 1.5f,
                         botLineH);
            g.DrawString(todoText.c_str(), -1, &fBot, rcTodo, &sfLeft, &todoBr);
        }

        // 右侧：标签
        {
            std::wstring tagsStr = s_Info.tagNames.empty()
                ? L"无标签"
                : JoinTags(s_Info.tagNames);
            Color tagCol = s_Info.tagNames.empty()
                ? Color(120, 160, 160, 160)
                : Color(200, 160, 210, 255);
            SolidBrush tagBr(tagCol);

            StringFormat sfRight;
            sfRight.SetAlignment(StringAlignmentFar);
            sfRight.SetLineAlignment(StringAlignmentCenter);
            sfRight.SetTrimming(StringTrimmingEllipsisCharacter);
            sfRight.SetFormatFlags(StringFormatFlagsNoWrap);

            RectF rcTag(halfW + padX * 0.5f,
                        botMidY - botLineH * 0.5f,
                        halfW - padX * 1.5f,
                        botLineH);
            g.DrawString(tagsStr.c_str(), -1, &fBot, rcTag, &sfRight, &tagBr);
        }

        // ── 关闭按钮（右上角）──
        float xFontSize = 12*sc;
        Font fX(g_MiSansFamily, xFontSize, FontStyleBold, UnitPixel);
        SolidBrush xBr(Color(150, 200, 200, 200));
        RectF rcX((REAL)(W - 22*sc), (REAL)(2*sc), 18*sc, 18*sc);
        StringFormat sfX;
        sfX.SetAlignment(StringAlignmentCenter);
        sfX.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"×", -1, &fX, rcX, &sfX, &xBr);
    }

    BLENDFUNCTION bf = {};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = g_OverlayAlpha;
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
    static bool  s_Resizing   = false;
    static POINT s_ResizeStart = {};
    static int   s_ResizeOrigW = 0;
    static int   s_ResizeOrigH = 0;
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

    // 关闭按钮
    if (pt.x >= W - 20 && pt.y <= 18) {
        HidePomodoroOverlay();
        return 0;
    }

    // Resize 边缘
    ResizeDir rd = GetResizeDir(hWnd, pt);
    if (rd != RD_NONE) {
        s_Resizing    = true;
        s_Dragging    = false;
        s_ResizeStart = pt;
        // 保存当前窗口物理尺寸（像素）
        RECT wr; GetWindowRect(hWnd, &wr);
        s_ResizeOrigW = wr.right  - wr.left;
        s_ResizeOrigH = wr.bottom - wr.top;
        SetCapture(hWnd);
        return 0;
    }

    // 拖拽移动
    s_Dragging  = true;
    // 改为屏幕坐标
    s_DragStart = pt;
    ClientToScreen(hWnd, &s_DragStart);  // ← 加这一行
    RECT wr; GetWindowRect(hWnd, &wr);
    s_WinStart.x = wr.left;
    s_WinStart.y = wr.top;
    SetCapture(hWnd);
    break;
    }

    case WM_MOUSEMOVE: {
    POINT cur = {LOWORD(lp), HIWORD(lp)};

    if (s_Resizing) {
        int dx = cur.x - s_ResizeStart.x;
        int dy = cur.y - s_ResizeStart.y;
        int newWpx = std::max(120, s_ResizeOrigW + dx);
        int newHpx = std::max(80,  s_ResizeOrigH + dy);

        g_OverlayWidth  = std::max(1, (int)(newWpx / g_Scale));
        g_OverlayHeight = std::max(1, (int)(newHpx / g_Scale));

        SetWindowPos(hWnd, NULL, 0, 0, newWpx, newHpx,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        OvRender();
        return 0;
    }

    if (s_Dragging) {
        // 改用屏幕坐标，避免窗口移动时客户区原点漂移导致计算错误
        POINT curScreen = cur;
        ClientToScreen(hWnd, &curScreen);
        int nx = s_WinStart.x + (curScreen.x - s_DragStart.x);
        int ny = s_WinStart.y + (curScreen.y - s_DragStart.y);
        SetWindowPos(hWnd, NULL, nx, ny, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_OverlayX = nx;
        g_OverlayY = ny;
        OvRender();
        return 0;
    }

    // 悬停时改变光标给用户视觉提示
    ResizeDir rd = GetResizeDir(hWnd, cur);
    switch (rd) {
        case RD_CORNER: SetCursor(LoadCursor(NULL, IDC_SIZENWSE)); break;
        case RD_RIGHT:  SetCursor(LoadCursor(NULL, IDC_SIZEWE));   break;
        case RD_BOTTOM: SetCursor(LoadCursor(NULL, IDC_SIZENS));   break;
        default:        SetCursor(LoadCursor(NULL, IDC_ARROW));    break;

    }
    // 在 WM_MOUSEMOVE 的 switch/光标部分之后、break 之前加：
    if (!s_Dragging && !s_Resizing) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize      = sizeof(tme);
        tme.dwFlags     = TME_HOVER | TME_LEAVE;
        tme.hwndTrack   = hWnd;
        tme.dwHoverTime = 50;  // 50ms 后触发 WM_MOUSEHOVER
        TrackMouseEvent(&tme);
    }
    break;
    }

    case WM_LBUTTONUP:
        if (s_Resizing) {
            s_Resizing = false;
            ReleaseCapture();
            SaveOverlaySettings();  // 持久化新尺寸
            return 0;
        }
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

    case WM_MOUSEHOVER: {
        // 鼠标悬停时临时允许接收输入（不弹到前台）
        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, style & ~WS_EX_NOACTIVATE);
        SetFocus(hWnd);
        break;
    }
    case WM_MOUSELEAVE: {
        // 鼠标离开时恢复
        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
        break;
    }

    case WM_MOUSEWHEEL: {
    int delta = GET_WHEEL_DELTA_WPARAM(wp);
    int newAlpha = (int)g_OverlayAlpha + (delta > 0 ? 15 : -15);
    newAlpha = std::max(30, std::min(255, newAlpha));
    g_OverlayAlpha = (BYTE)newAlpha;
    OvRender();
    SaveOverlaySettings();
    return 0;
    }

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
    SaveOverlaySettings();  // 临时加，验证能否写入
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
static std::wstring GetIniPath() {
    WCHAR buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    PathAppendW(buf, SETTINGS_FILE.c_str());
    return std::wstring(buf);
}

void SaveOverlaySettings() {
    std::wstring path = GetIniPath();  // ← 改这里
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
    std::wstring path = GetIniPath();  // ← 改这里
    g_OverlayAlpha    = (BYTE)GetPrivateProfileIntW(L"Overlay", L"alpha",    200,   path.c_str());
    g_OverlayX        = GetPrivateProfileIntW(L"Overlay", L"x",        -1,    path.c_str());
    g_OverlayY        = GetPrivateProfileIntW(L"Overlay", L"y",        -1,    path.c_str());
    g_OverlayFontSize = GetPrivateProfileIntW(L"Overlay", L"fontSize", 32,    path.c_str());
    g_OverlayWidth    = GetPrivateProfileIntW(L"Overlay", L"width",    OV_W,  path.c_str());
    g_OverlayHeight   = GetPrivateProfileIntW(L"Overlay", L"height",   OV_H,  path.c_str());
}
