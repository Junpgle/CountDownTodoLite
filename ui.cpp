#include "ui.h"
#include "utils.h"
#include "api.h"
#include "tai_reader.h"

using namespace Gdiplus;

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
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HBITMAP hOld = (HBITMAP) SelectObject(hdcMem, hBitmap); {
        Graphics g(hdcMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(0, 0, 0, 0));

        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_HitZones.clear();

        int r = S(15);
        GraphicsPath path;
        path.AddArc(0, 0, r * 2, r * 2, 180, 90);
        path.AddArc(width - r * 2, 0, r * 2, r * 2, 270, 90);
        path.AddArc(width - r * 2, height - r * 2, r * 2, r * 2, 0, 90);
        path.AddArc(0, height - r * 2, r * 2, r * 2, 90, 90);
        path.CloseFigure();
        SolidBrush bg(Color(g_BgAlpha, 25, 25, 25));
        g.FillPath(&bg, &path);

        FontFamily ff(L"MiSans");
        Font titleF(&ff, (REAL) S(16), FontStyleBold, UnitPixel);
        Font headF(&ff, (REAL) S(12), FontStyleRegular, UnitPixel);
        SolidBrush wBrush(Color(255, 255, 255, 255));
        SolidBrush gBrush(Color(255, 180, 180, 180));
        SolidBrush grBrush(Color(255, 80, 220, 80));
        SolidBrush rBrush(Color(255, 255, 100, 100));
        Pen linePen(Color(100, 255, 255, 255), 1);

        g.DrawString((L"Hello, " + g_Username).c_str(), -1, &titleF, PointF((REAL) S(15), (REAL) S(15)), &wBrush);
        g.DrawLine(&linePen, S(15), S(45), width - S(15), S(45));

        float y = (float) S(55);
        Font contentF(&ff, (REAL) S(14), FontStyleRegular, UnitPixel);

        // --- 1. 倒计时 ---
        g.DrawString(L"\u5012\u8ba1\u65f6", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 2});
        y += S(20);
        for (const auto &it: g_Countdowns) {
            g_HitZones.push_back({Rect(S(15), (int) y, width - S(30), S(20)), it.id, 4});
            g.DrawString(it.title.c_str(), -1, &contentF, PointF((REAL) S(15), y), &wBrush);
            std::wstring d = std::to_wstring(it.daysLeft) + L" \u5929";
            g.DrawString(d.c_str(), -1, &contentF, PointF((REAL) (width - S(60)), y), it.daysLeft <= 3 ? &rBrush : &grBrush);
            y += S(20);
        }

        // --- 2. 屏幕使用时间统计 (读取 Tai 数据) ---
        y += S(10);
        g.DrawString(L"\u4eca\u65e5\u5c4f\u5e55\u65f6\u95f4", -1, &headF, PointF((REAL) S(15), y), &gBrush); // "今日屏幕时间"
        y += S(20);

        auto topApps = GetTopApps(3); // 显示前三的应用
        if (topApps.empty()) {
            g.DrawString(L"\u6682\u65e0\u6570\u636e", -1, &contentF, PointF((REAL) S(15), y), &gBrush); // "暂无数据"
            y += S(20);
        } else {
            for (const auto& app : topApps) {
                int sec = app.second;
                std::wstring timeStr;
                if (sec < 60) {
                    timeStr = L"<1 \u5206\u949f"; // "<1 分钟"
                } else if (sec < 3600) {
                    timeStr = std::to_wstring(sec / 60) + L" \u5206\u949f";
                } else {
                    timeStr = std::to_wstring(sec / 3600) + L" \u5c0f\u65f6 " + std::to_wstring((sec % 3600) / 60) + L" \u5206";
                }

                // 处理过长的应用名称，防止UI重叠
                std::wstring dispName = app.first;
                if (dispName.length() > 15) dispName = dispName.substr(0, 13) + L"...";

                g.DrawString(dispName.c_str(), -1, &contentF, PointF((REAL) S(15), y), &wBrush);
                g.DrawString(timeStr.c_str(), -1, &contentF, PointF((REAL) (width - S(90)), y), &grBrush);
                y += S(20);
            }
        }

        // --- 3. 待办事项 ---
        y += S(10);
        g.DrawString(L"\u5f85\u529e\u4e8b\u9879", -1, &headF, PointF((REAL) S(15), y), &gBrush);
        g.DrawString(L"[+]", -1, &headF, PointF((REAL) (width - S(40)), y), &grBrush);
        g_HitZones.push_back({Rect(width - S(45), (int) y, S(30), S(20)), 0, 1});
        y += S(20);
        if (g_Todos.empty()) {
            g.DrawString(L"\u6682\u65e0\u5f85\u529e", -1, &contentF, PointF((REAL) S(15), y), &gBrush);
        } else {
            for (const auto &it: g_Todos) {
                g_HitZones.push_back({Rect(S(15), (int) y, width - S(30), S(20)), it.id, 3});
                g.DrawRectangle(&linePen, S(15), (int) y + S(4), S(10), S(10));
                if (it.isDone) g.FillRectangle(&wBrush, S(17), (int) y + S(6), S(6), S(6));

                int style = it.isDone ? FontStyleStrikeout : FontStyleRegular;
                SolidBrush dBrush(Color(100, 255, 255, 255));
                Font itemF(&ff, (REAL) S(14), style, UnitPixel);
                g.DrawString(it.content.c_str(), -1, &itemF, PointF((REAL) S(30), y), it.isDone ? &dBrush : &wBrush);
                y += S(20);
            }
        }
    }

    POINT ptSrc = {0, 0};
    POINT ptDest = {rc.left, rc.top};
    SIZE sz = {width, height};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hWidgetWnd, hdcScreen, &ptDest, &sz, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);
    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void ResizeWidget() {
    if (!g_hWidgetWnd) return;
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

    // 基础高度计算
    int h = S(55) + S(20) + S(10) + S(20) + S(10);

    // 倒计时高度
    h += (int) g_Countdowns.size() * S(20);

    // 屏幕时间高度计算 (标题 + 内容)
    h += S(10) + S(20);
    int appsCount = std::min(3, (int)g_AppUsage.size());
    h += (appsCount == 0 ? 1 : appsCount) * S(20);

    // 待办高度
    h += (g_Todos.empty() ? 1 : (int) g_Todos.size()) * S(20);

    // 补充边距
    h += S(20);

    if (h < S(200)) h = S(200);
    RECT rc;
    GetWindowRect(g_hWidgetWnd, &rc);
    SetWindowPos(g_hWidgetWnd, HWND_BOTTOM, rc.left, rc.top, S(300), h, SWP_NOACTIVATE);
    RenderWidget();
}

LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR buf[256];
        GetDlgItemTextW(hWnd, 101, buf, 256);
        InputState::result1 = buf;
        if (InputState::currentType == 0) {
            int idx = SendMessageW(GetDlgItem(hWnd, 105), CB_GETCURSEL, 0, 0);
            if (idx > 0) {
                WCHAR c[20], n[20];
                SendMessageW(GetDlgItem(hWnd, 105), CB_GETLBTEXT, idx, (LPARAM) c);
                GetDlgItemTextW(hWnd, 102, n, 20);
                InputState::result1 += L" [" + std::wstring(c) + L", " + n + L"\u6b21]";
            }
        } else {
            GetDlgItemTextW(hWnd, 102, buf, 256);
            InputState::result2 = buf;
        }
        InputState::isOk = !InputState::result1.empty();
        DestroyWindow(hWnd);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowInputDialog(HWND parent, int type, std::wstring &out1, std::wstring &out2) {
    InputState::isOk = false;
    InputState::currentType = type;
    const wchar_t CN[] = L"InputDlgClass";
    UnregisterClassW(CN, g_hInst);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = CN;
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HFONT hF = GetMiSansFont(14);

    std::wstring title = type == 0 ? L"\u6dfb\u52a0\u5f85\u529e\u4e8b\u9879" : L"\u6dfb\u52a0\u5012\u8ba1\u65f6";
    HWND hDlg = CreateWindowW(CN, title.c_str(), WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, S(320), type==0?S(240):S(200), parent, NULL, g_hInst, NULL);

    std::wstring label1 = type == 0 ? L"\u5185\u5bb9\u003a" : L"\u6807\u9898\u003a";
    HWND hL1 = CreateWindowW(L"STATIC", label1.c_str(), WS_VISIBLE|WS_CHILD, S(20), S(20), S(50), S(20), hDlg, NULL, g_hInst, NULL);
    SendMessage(hL1, WM_SETFONT, (WPARAM) hF, 1);
    HWND hE1 = CreateWindowW(L"EDIT", L"", WS_VISIBLE|WS_CHILD|WS_BORDER, S(80), S(20), S(200), S(20), hDlg, (HMENU)101, g_hInst, NULL);
    SendMessage(hE1, WM_SETFONT, (WPARAM) hF, 1);

    if (type == 0) {
        CreateWindowW(L"STATIC", L"\u91cd\u590d\u003a", WS_VISIBLE|WS_CHILD, S(20), S(55), S(50), S(20), hDlg, NULL, g_hInst, NULL);
        HWND hC = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE|WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL, S(80), S(50), S(200), S(120), hDlg, (HMENU)105, g_hInst, NULL);
        SendMessage(hC, WM_SETFONT, (WPARAM) hF, 1);
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u4e0d\u91cd\u590d");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u5929");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u5468");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u6708");
        SendMessage(hC, CB_ADDSTRING, 0, (LPARAM) L"\u6bcf\u5e74");
        SendMessage(hC, CB_SETCURSEL, 0, 0);
        CreateWindowW(L"STATIC", L"\u6b21\u6570\u003a", WS_VISIBLE|WS_CHILD, S(20), S(85), S(50), S(20), hDlg, NULL, g_hInst, NULL);
        HWND hE2 = CreateWindowW(L"EDIT", L"1", WS_VISIBLE|WS_CHILD|WS_BORDER|ES_NUMBER, S(80), S(85), S(200), S(20), hDlg, (HMENU)102, g_hInst, NULL);
        SendMessage(hE2, WM_SETFONT, (WPARAM) hF, 1);
    } else {
        CreateWindowW(L"STATIC", L"\u65e5\u671f\u003a", WS_VISIBLE|WS_CHILD, S(20), S(50), S(50), S(20), hDlg, NULL, g_hInst, NULL);
        SYSTEMTIME st; GetLocalTime(&st); wchar_t db[20]; wsprintfW(db, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        HWND hE2 = CreateWindowW(L"EDIT", db, WS_VISIBLE|WS_CHILD|WS_BORDER, S(80), S(50), S(200), S(20), hDlg, (HMENU)102, g_hInst, NULL);
        SendMessage(hE2, WM_SETFONT, (WPARAM) hF, 1);
    }
    HWND hBtn = CreateWindowW(L"BUTTON", L"\u786e\u5b9a", WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON, S(110), type==0?S(125):S(100), S(80), S(30), hDlg, (HMENU)IDOK, g_hInst, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM) hF, 1);

    EnableWindow(parent, 0);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessage(hDlg, WM_COMMAND, IDOK, 0); continue;
        }
        TranslateMessage(&msg); DispatchMessage(&msg);
        if (!IsWindow(hDlg)) break;
    }
    EnableWindow(parent, 1); SetForegroundWindow(parent); DeleteObject(hF);
    out1 = InputState::result1; out2 = InputState::result2;
    return InputState::isOk;
}

LRESULT CALLBACK LoginWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    using json = nlohmann::json;
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        WCHAR e[100], p[100]; GetWindowTextW(g_hEmail, e, 100); GetWindowTextW(g_hPass, p, 100);
        bool rem = SendMessage(g_hAutoLogin, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (lstrlenW(e) == 0 || lstrlenW(p) == 0) { MessageBoxW(hWnd, L"\u8bf7\u8f93\u5165\u90ae\u7bb1\u548c\u5bc6\u7801", L"\u63d0\u793a", 0); return 0; }
        SetCursor(LoadCursor(0, IDC_WAIT));
        std::string res = SendRequest(L"/api/auth/login", "POST", "{\"email\":\"" + ToUtf8(e) + "\",\"password\":\"" + ToUtf8(p) + "\"}");
        SetCursor(LoadCursor(0, IDC_ARROW));
        if (res.empty() || res.find("ERROR:") == 0) {
            MessageBoxW(hWnd, (L"\u7f51\u7edc\u9519\u8bef (" + ToWide(res) + L")").c_str(), L"Error", 16); return 0;
        }
        try {
            auto j = json::parse(res);
            if (j["success"].get<bool>()) {
                g_UserId = j["user"]["id"]; g_Username = ToWide(j["user"]["username"]);
                SaveSettings(g_UserId, g_Username, e, rem ? p : L"", rem); g_LoginSuccess = true; DestroyWindow(hWnd);
            } else { MessageBoxW(hWnd, ToWide(j.contains("message") ? j["message"] : "Unknown error").c_str(), L"\u767b\u5f55\u5931\u8d25", 16); }
        } catch (...) { MessageBoxW(hWnd, L"\u89e3\u6790\u54cd\u5e94\u5931\u8d25", L"Error", 16); }
    }
    if (msg == WM_CLOSE) DestroyWindow(hWnd);
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ShowLogin() {
    const wchar_t CN[] = L"LoginClass";
    WNDCLASSW wc = {0}; wc.lpfnWndProc = LoginWndProc; wc.hInstance = g_hInst; wc.lpszClassName = CN; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc);
    HWND h = CreateWindowW(CN, L"Math Quiz Login", WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE, (GetSystemMetrics(0)-S(300))/2, (GetSystemMetrics(1)-S(220))/2, S(300), S(220), 0, 0, g_hInst, 0);
    HFONT f = GetMiSansFont(14);
    CreateWindowW(L"STATIC", L"Email:", WS_VISIBLE|WS_CHILD, S(20), S(20), S(60), S(20), h, 0, g_hInst, 0);
    g_hEmail = CreateWindowW(L"EDIT", g_SavedEmail.c_str(), WS_VISIBLE|WS_CHILD|WS_BORDER, S(80), S(20), S(180), S(20), h, 0, g_hInst, 0);
    CreateWindowW(L"STATIC", L"Pass:", WS_VISIBLE|WS_CHILD, S(20), S(50), S(60), S(20), h, 0, g_hInst, 0);
    g_hPass = CreateWindowW(L"EDIT", L"", WS_VISIBLE|WS_CHILD|WS_BORDER|ES_PASSWORD, S(80), S(50), S(180), S(20), h, 0, g_hInst, 0);
    g_hAutoLogin = CreateWindowW(L"BUTTON", L"\u81ea\u52a8\u767b\u5f55 / \u8bb0\u4f4f\u5bc6\u7801", WS_VISIBLE|WS_CHILD|BS_AUTOCHECKBOX, S(80), S(80), S(180), S(20), h, (HMENU)103, g_hInst, 0);
    SendMessage(g_hAutoLogin, BM_SETCHECK, BST_CHECKED, 0);
    CreateWindowW(L"BUTTON", L"Login", WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON, S(100), S(120), S(80), S(30), h, (HMENU)IDOK, g_hInst, 0);
    EnumChildWindows(h, [](HWND c, LPARAM p) { SendMessage(c, WM_SETFONT, (WPARAM) p, 1); return TRUE; }, (LPARAM) f);
    MSG m; while (GetMessage(&m, 0, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    DeleteObject(f); return g_LoginSuccess;
}

LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 60000, NULL);
            CreateThread(NULL, 0, [](LPVOID) { SyncData(); return (DWORD) 0; }, NULL, 0, NULL);
            break;
        case WM_TIMER:
            CreateThread(NULL, 0, [](LPVOID) { SyncData(); return (DWORD) 0; }, NULL, 0, NULL);
            break;
        case WM_USER_REFRESH:
        case WM_USER_TICK:
            ResizeWidget(); // 内部包含了 RenderWidget
            break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp), y = HIWORD(lp); bool hit = false;
            std::vector<HitZone> zonesCopy;
            { std::lock_guard<std::recursive_mutex> lock(g_DataMutex); zonesCopy = g_HitZones; }

            for (const auto &z: zonesCopy) {
                if (z.rect.Contains(x, y)) {
                    hit = true;
                    if (z.type == 1) {
                        std::wstring c, d;
                        if (ShowInputDialog(hWnd, 0, c, d)) {
                            { std::lock_guard<std::recursive_mutex> lock(g_DataMutex); g_Todos.insert(g_Todos.begin(), {-1, c, false, std::time(nullptr)}); }
                            ResizeWidget();
                            CreateThread(NULL, 0, [](LPVOID p) { std::wstring *s = (std::wstring *) p; ApiAddTodo(*s); SyncData(); delete s; return (DWORD) 0; }, new std::wstring(c), 0, NULL);
                        }
                    } else if (z.type == 2) {
                        std::wstring t, d;
                        if (ShowInputDialog(hWnd, 1, t, d)) {
                            { std::lock_guard<std::recursive_mutex> lock(g_DataMutex); g_Countdowns.push_back({-1, t, d, CalculateDaysLeft(d), std::time(nullptr)}); }
                            ResizeWidget();
                            struct Ctx { std::wstring t, d; };
                            CreateThread(NULL, 0, [](LPVOID p) { Ctx *c = (Ctx *) p; ApiAddCountdown(c->t, c->d); SyncData(); delete c; return (DWORD) 0; }, new Ctx{t, d}, 0, NULL);
                        }
                    } else if (z.type == 3) {
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        for (auto &t: g_Todos) {
                            if (t.id == z.id) {
                                t.isDone = !t.isDone; t.lastUpdated = std::time(nullptr);
                                PostMessage(hWnd, WM_USER_REFRESH, 0, 0);
                                if (t.id > 0) CreateThread(NULL, 0, [](LPVOID p) { auto *pair = (std::pair<int, bool> *) p; ApiToggleTodo(pair->first, pair->second); delete pair; return (DWORD) 0; }, new std::pair<int, bool>(t.id, t.isDone), 0, NULL);
                                break;
                            }
                        }
                    }
                    break;
                }
            }
            if (!hit) SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } break;
        case WM_RBUTTONUP: {
            POINT pt; GetCursorPos(&pt); HMENU hMenu = CreatePopupMenu();
            int selId = 0, selType = 0;
            { std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
              int x = LOWORD(lp), y = HIWORD(lp);
              for (const auto &z: g_HitZones) if (z.rect.Contains(x, y) && (z.type == 3 || z.type == 4)) { selId = z.id; selType = z.type; break; }
            }
            if (selId > 0) AppendMenuW(hMenu, 0, 1001, L"\u5220\u9664\u6b64\u9879"); // "删除此项"

            HMENU hSub = CreatePopupMenu();
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 50 ? MF_CHECKED : 0), 2001, L"20%");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 100 ? MF_CHECKED : 0), 2002, L"40% (\u9ed8\u8ba4)");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 150 ? MF_CHECKED : 0), 2003, L"60%");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 200 ? MF_CHECKED : 0), 2004, L"80%");
            AppendMenuW(hSub, MF_STRING | (g_BgAlpha == 255 ? MF_CHECKED : 0), 2005, L"100% (\u4e0d\u900f\u660e)");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR) hSub, L"\u80cc\u666f\u900f\u660e\u5ea6");

            AppendMenuW(hMenu, 0, 1005, L"\u8bbe\u7f6e Tai \u6570\u636e\u5e93\u8def\u5f84"); // 设置 Tai 数据库路径
            AppendMenuW(hMenu, 0, 1002, L"\u7acb\u5373\u5237\u65b0");
            AppendMenuW(hMenu, 0, 1004, L"\u9000\u51fa\u7a0b\u5e8f");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
            if (cmd == 1001) {
                { std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                  if (selType == 3) { for (auto it = g_Todos.begin(); it != g_Todos.end(); ++it) if (it->id == selId) { g_Todos.erase(it); break; } }
                  else if (selType == 4) { for (auto it = g_Countdowns.begin(); it != g_Countdowns.end(); ++it) if (it->id == selId) { g_Countdowns.erase(it); break; } }
                }
                ResizeWidget();
                if (selType == 3) CreateThread(NULL, 0, [](LPVOID p) { ApiDeleteTodo((int) (uintptr_t) p); return (DWORD) 0; }, (LPVOID) (uintptr_t) selId, 0, NULL);
                if (selType == 4) CreateThread(NULL, 0, [](LPVOID p) { ApiDeleteCountdown((int) (uintptr_t) p); return (DWORD) 0; }, (LPVOID) (uintptr_t) selId, 0, NULL);
            } else if (cmd >= 2001 && cmd <= 2005) {
                if (cmd == 2001) g_BgAlpha = 50; else if (cmd == 2002) g_BgAlpha = 100; else if (cmd == 2003) g_BgAlpha = 150; else if (cmd == 2004) g_BgAlpha = 200; else g_BgAlpha = 255;
                SaveAlphaSetting(); RenderWidget();
            } else if (cmd == 1005) {
                OPENFILENAMEW ofn;
                WCHAR szFile[MAX_PATH] = {0};
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile);
                ofn.lpstrFilter = L"SQLite 数据库\0*.db\0所有文件\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn) == TRUE) {
                    g_TaiDbPath = szFile;
                    SaveTaiDbPathSetting();
                    MessageBoxW(hWnd, L"Tai \u6570\u636e\u5e93\u8def\u5f84\u5df2\u66f4\u65b0\uff01", L"\u63d0\u793a", MB_OK); // "Tai 数据库路径已更新！", "提示"
                }
            } else if (cmd == 1002) CreateThread(NULL, 0, [](LPVOID) { SyncData(); return (DWORD) 0; }, NULL, 0, NULL);
            else if (cmd == 1004) PostQuitMessage(0);
            DestroyMenu(hMenu);
        } break;
        case WM_PAINT: ValidateRect(hWnd, NULL); return 0;
        case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}