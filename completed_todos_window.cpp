#include "ui.h"
#include "utils.h"
#include "api.h"
#include <vector>
#include <string>
#include <algorithm>

using namespace Gdiplus;

// --- 状态与结构 ---
struct CompHitZone {
    RectF rect;
    int id;
    int action; // 1: 恢复, 2: 删除
};

static int g_CompScrollY = 0;
static int g_CompMaxScrollY = 0;
static std::vector<CompHitZone> g_CompHitZones;

// 外部引用
extern void SyncData();
extern std::vector<Todo> g_Todos;
extern std::recursive_mutex g_DataMutex;
// 注意：移除了 extern int S(int v); 因为 S(x) 是在 common.h 定义的宏

// 辅助：绘制圆角矩形路径
static void AddRoundedRectToPath_Comp(GraphicsPath& path, RectF rect, REAL radius) {
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

// 核心绘制逻辑
void DrawCompletedTodos(Graphics& g, int width, int height) {
    Color bgColor = Color(255, 245, 247, 250);
    Color cardBgColor = Color(255, 255, 255, 255);
    Color textColor = Color(255, 44, 62, 80);
    Color subTextColor = Color(255, 127, 140, 141);
    Color accentColor = Color(255, 74, 108, 247);
    Color redColor = Color(255, 231, 76, 60);
    Color greenColor = Color(255, 46, 204, 113);

    g.Clear(bgColor);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    FontFamily ff(L"MiSans");
    Font titleF(&ff, (REAL)S(22), FontStyleBold, UnitPixel);
    Font normalF(&ff, (REAL)S(16), FontStyleRegular, UnitPixel);
    Font strikeF(&ff, (REAL)S(16), FontStyleStrikeout, UnitPixel);
    Font smallF(&ff, (REAL)S(13), FontStyleRegular, UnitPixel);

    SolidBrush textBrush(textColor);
    SolidBrush subBrush(subTextColor);
    SolidBrush cardBrush(cardBgColor);
    SolidBrush accentBrush(accentColor);
    SolidBrush redBrush(redColor);

    StringFormat centerF;
    centerF.SetAlignment(StringAlignmentCenter);
    centerF.SetLineAlignment(StringAlignmentCenter);

    std::vector<Todo> compTodos;
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_CompHitZones.clear();
        for (const auto& t : g_Todos) {
            if (t.isDone) compTodos.push_back(t);
        }
    }

    // 1. 固定头部
    g.DrawString(L"已完成事项", -1, &titleF, PointF((REAL)S(30), (REAL)S(25)), &textBrush);
    std::wstring countStr = L"共 " + std::to_wstring(compTodos.size()) + L" 项记录";
    g.DrawString(countStr.c_str(), -1, &normalF, PointF((REAL)S(160), (REAL)S(32)), &subBrush);

    // 2. 滚动区域截断与坐标变换
    float headerHeight = (float)S(80);
    RectF viewport(0, headerHeight, (REAL)width, (REAL)(height - headerHeight));
    Region clipRegion(viewport);
    g.SetClip(&clipRegion);

    GraphicsState state = g.Save();
    g.TranslateTransform(0, (REAL)-g_CompScrollY);

    float y = headerHeight;
    float cardW = (float)width - S(60);
    float cardH = (float)S(90);

    if (compTodos.empty()) {
        g.DrawString(L"干净清爽，暂无已完成的事项。", -1, &normalF, PointF((REAL)S(30), y + S(20)), &subBrush);
    } else {
        for (const auto& it : compTodos) {
            RectF cardR((REAL)S(30), y, cardW, cardH);
            GraphicsPath cp;
            AddRoundedRectToPath_Comp(cp, cardR, (REAL)S(12));
            g.FillPath(&cardBrush, &cp);

            // 绘制勾选状态图标 (绿色圆环加对号)
            SolidBrush checkBrush(greenColor);
            g.FillEllipse(&checkBrush, cardR.X + S(20), cardR.Y + S(25), (REAL)S(24), (REAL)S(24));
            Pen whitePen(Color::White, 2.0f);
            g.DrawLine(&whitePen, cardR.X + S(26), cardR.Y + S(37), cardR.X + S(30), cardR.Y + S(42));
            g.DrawLine(&whitePen, cardR.X + S(30), cardR.Y + S(42), cardR.X + S(38), cardR.Y + S(31));

            // 待办内容文本 (带删除线)
            g.DrawString(it.content.c_str(), -1, &strikeF, PointF(cardR.X + S(60), cardR.Y + S(25)), &subBrush);

            // 日期信息
            std::wstring dateStr = L"创建: " + it.createdDate;
            if (!it.dueDate.empty()) dateStr += L"  |  截止: " + it.dueDate;
            g.DrawString(dateStr.c_str(), -1, &smallF, PointF(cardR.X + S(60), cardR.Y + S(55)), &subBrush);

            // 操作按钮：恢复与删除
            RectF btnRest(cardR.X + cardR.Width - S(170), cardR.Y + S(27), (REAL)S(70), (REAL)S(36));
            RectF btnDel(cardR.X + cardR.Width - S(90), cardR.Y + S(27), (REAL)S(70), (REAL)S(36));

            GraphicsPath pRest, pDel;
            AddRoundedRectToPath_Comp(pRest, btnRest, (REAL)S(8));
            AddRoundedRectToPath_Comp(pDel, btnDel, (REAL)S(8));

            // 恢复按钮 (浅蓝底色 + 深蓝字)
            SolidBrush restBg(Color(30, 74, 108, 247));
            g.FillPath(&restBg, &pRest);
            g.DrawString(L"恢复", -1, &smallF, btnRest, &centerF, &accentBrush);
            g_CompHitZones.push_back(CompHitZone{btnRest, it.id, 1}); // 显式初始化

            // 删除按钮 (浅红底色 + 深红字)
            SolidBrush delBg(Color(30, 231, 76, 60));
            g.FillPath(&delBg, &pDel);
            g.DrawString(L"删除", -1, &smallF, btnDel, &centerF, &redBrush);
            g_CompHitZones.push_back(CompHitZone{btnDel, it.id, 2}); // 显式初始化

            y += cardH + S(15);
        }
    }

    // 计算最大滚动距离
    g_CompMaxScrollY = (int)(y - height + S(30));
    if (g_CompMaxScrollY < 0) g_CompMaxScrollY = 0;

    g.Restore(state);
    g.ResetClip();
}

LRESULT CALLBACK CompletedTodosWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_CompScrollY = 0;
            break;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_CompScrollY -= delta / 2;
            if (g_CompScrollY < 0) g_CompScrollY = 0;
            if (g_CompScrollY > g_CompMaxScrollY) g_CompScrollY = g_CompMaxScrollY;
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp), y = HIWORD(lp);
            float headerHeight = (float)S(80);

            // 仅当点击位置在滚动区域内时检测按钮
            if (y >= headerHeight) {
                float adjustedY = y + g_CompScrollY;
                int actionId = 0;
                int targetTodoId = -1;

                {
                    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                    for (const auto& z : g_CompHitZones) {
                        if (z.rect.Contains((REAL)x, adjustedY)) {
                            targetTodoId = z.id;
                            actionId = z.action;
                            break;
                        }
                    }
                }

                if (actionId == 1) { // 恢复
                    // 乐观更新 UI 以获得即时反馈
                    {
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        for(auto& t : g_Todos) if(t.id == targetTodoId) t.isDone = false;
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                    // 后台发起 API 同步
                    ApiToggleTodo(targetTodoId, false);
                    SyncData();
                }
                else if (actionId == 2) { // 彻底删除
                    if (MessageBoxW(hWnd, L"确定要彻底删除这条已完成的事项吗？", L"确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        {
                            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                            g_Todos.erase(std::remove_if(g_Todos.begin(), g_Todos.end(), [targetTodoId](const auto& t){ return t.id == targetTodoId; }), g_Todos.end());
                        }
                        InvalidateRect(hWnd, NULL, FALSE);
                        ApiDeleteTodo(targetTodoId);
                        SyncData();
                    }
                }
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);

            // 双缓冲防闪烁绘图
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mdc, mbm);

            Graphics graphics(mdc);
            DrawCompletedTodos(graphics, rc.right, rc.bottom);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old);
            DeleteObject(mbm);
            DeleteDC(mdc);
            EndPaint(hWnd, &ps);
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hWnd);
            break;

        default:
            return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

void ShowCompletedTodosWindow(HWND parent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = CompletedTodosWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"CompletedTodosWndModernClass";
        RegisterClassW(&wc);
        registered = true;
    }

    // 初始化窗口大小 (适中的弹窗大小)
    int winWidth = S(650);
    int winHeight = S(600);
    int x = (GetSystemMetrics(SM_CXSCREEN) - winWidth) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - winHeight) / 2;

    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"CompletedTodosWndModernClass", L"已完成的待办事项",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, winWidth, winHeight, parent, NULL, GetModuleHandle(NULL), NULL);

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
}