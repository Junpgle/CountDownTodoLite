# MathQuizLite — 项目架构文档

> **文档用途**：本文档是为 AI 辅助开发而生成的上下文速查手册。新开 AI 会话时，将此文档作为上下文输入，AI 即可立刻理解项目的全局结构、编码规范与开发约定，无需重新阅读全部源码。
>
> **最后更新**：2026-03-07

---

## 目录

1. [项目概述](#1-项目概述)
2. [技术栈全览](#2-技术栈全览)
3. [目录结构与文件职责](#3-目录结构与文件职责)
4. [启动流程](#4-启动流程)
5. [核心架构：三层模型](#5-核心架构三层模型)
6. [开发规范：如何调用 API 接口](#6-开发规范如何调用-api-接口)
7. [开发规范：如何新增一个弹出窗口（页面）](#7-开发规范如何新增一个弹出窗口页面)
8. [开发规范：如何新增全局状态变量](#8-开发规范如何新增全局状态变量)
9. [开发规范：UI 绘制约定](#9-开发规范ui-绘制约定)
10. [数据持久化约定](#10-数据持久化约定)
11. [线程安全约定](#11-线程安全约定)
12. [已知技术债与注意事项](#12-已知技术债与注意事项)
13. [构建与编译](#13-构建与编译)

---

## 1. 项目概述

**MathQuizLite** 是 CountDownTodo 生态的 **Windows 桌面端悬浮组件**，以半透明悬浮窗的形式常驻桌面，提供以下功能：

- 待办事项（Todo）的增删改查与云端同步
- 重要日倒计时管理
- 多端屏幕时间统计（通过读取 [Tai](https://github.com/Cimpler/Tai) 软件的本地 SQLite 数据库采集 Windows 端数据，再与移动端上传的数据云端聚合）
- 课程表周视图（从云端拉取课程数据并展示）
- 程序自启、自动登录、静默更新检查

**生态关系**：
```
[Flutter 移动端] ──┐
                   ├──► [Cloudflare Workers 后端 API] ◄── [本项目：Windows 桌面端 C++]
[Web 管理端(可选)]──┘
```

后端仓库：[CountDownTodo](https://github.com/Junpgle/CountDownTodo)（含 Flutter 移动端 + Cloudflare Workers 后端）

---

## 2. 技术栈全览

| 类别 | 技术 | 说明 |
|------|------|------|
| **语言** | C++ 17 | MSVC / MinGW-w64 编译 |
| **构建系统** | CMake 3.10+ | `CMakeLists.txt` 管理 |
| **UI 渲染** | GDI+ (`gdiplus.h`) | 纯手工绘制，无任何 GUI 框架 |
| **窗口系统** | Win32 API | `CreateWindowExW`、消息循环、`WndProc` 回调 |
| **主窗口渲染模式** | `UpdateLayeredWindow` | 分层窗口，支持逐像素 ARGB 透明，**不走 WM_PAINT** |
| **弹窗渲染模式** | `WM_PAINT` + 双缓冲 `BitBlt` | 普通窗口，标准 GDI+ 绘制 |
| **网络请求** | WinHTTP | 原生 Windows HTTPS，强制 TLS 1.2/1.3 |
| **JSON 解析** | nlohmann/json v3.x | 单头文件 `json.hpp`，`using json = nlohmann::json;` |
| **本地数据库** | SQLite3（Amalgamation） | 仅用于读取 Tai 屏幕时间数据，`sqlite3.c / sqlite3.h` |
| **密码加密** | Windows DPAPI | `CryptProtectData` / `CryptUnprotectData`，与 Windows 账户绑定 |
| **配置持久化** | Windows INI 文件 | `WritePrivateProfileStringW` / `GetPrivateProfileStringW` |
| **字体** | Microsoft YaHei（系统字体） | GDI+ `FontFamily(L"Microsoft YaHei")`，无需外部字体文件 |
| **DPI 适配** | 宏 `S(x)` | `#define S(x) ((int)((x) * g_Scale))`，全局统一缩放 |
| **后端 API** | 自建 REST（HTTPS） | Host：`mathquiz.junpgle.me`，定义在 `common.cpp` |

---

## 3. 目录结构与文件职责

```
MathQuizLite/
│
├── main.cpp                      # 程序入口 (WinMain)
│                                 #   - GDI+ 初始化与关闭
│                                 #   - DPI 缩放获取
│                                 #   - 调用 LoadSettings() 加载配置
│                                 #   - 调用 ShowLogin() 处理登录/自动登录
│                                 #   - 创建主悬浮窗 (MathWidget)
│                                 #   - 启动 SyncData 子线程和更新检查
│                                 #   - Win32 主消息循环
│
├── common.h                      # 全局共享头文件（所有 .cpp 都包含）
│                                 #   - 所有 struct 定义 (Todo, Countdown, Course 等)
│                                 #   - 所有全局变量的 extern 声明
│                                 #   - 自定义消息宏 (WM_USER_REFRESH, WM_USER_TICK)
│                                 #   - DPI 宏 S(x)
│
├── common.cpp                    # 全局变量的实际内存分配与初始化
│                                 #   - API_HOST、SETTINGS_FILE 常量
│                                 #   - 所有 g_xxx 变量的定义
│
├── api.h / api.cpp               # 网络层（项目的"Service 层"）
│                                 #   - SendRequest()：底层 WinHTTP 封装
│                                 #   - ApiLogin()、ApiAddTodo()、SyncData() 等业务 API
│                                 #   - AttemptAutoLogin()
│                                 #   - SaveLocalCourses() / LoadLocalCourses() 本地缓存
│
├── utils.h / utils.cpp           # 工具函数层
│                                 #   - ToUtf8() / ToWide()：字符串编码转换
│                                 #   - EncryptString() / DecryptString()：DPAPI 加密
│                                 #   - LoadSettings() / SaveSettings()：INI 读写
│                                 #   - GetMiSansFont()：创建 GDI 字体句柄
│                                 #   - GetTodayDate()：获取今日日期字符串
│                                 #   - CalculateDaysLeft()：计算倒计时天数
│
├── ui.h / ui.cpp                 # UI 层（核心，最大的文件）
│                                 #   - RenderWidget()：主悬浮窗每帧绘制
│                                 #   - ResizeWidget()：根据内容动态调整窗口高度
│                                 #   - WidgetWndProc()：主窗口消息处理（点击/右键菜单）
│                                 #   - ShowLogin() / LoginWndProc()：登录窗口
│                                 #   - ShowInputDialog() / InputWndProc()：通用输入弹窗
│                                 #   - LoadLoginConfig() / SaveLoginConfig()：登录配置读写
│
├── tai_reader.h / tai_reader.cpp # 屏幕时间采集层
│                                 #   - StartTaiReader() / StopTaiReader()：管理后台采集线程
│                                 #   - 读取 Tai 软件的 SQLite 数据库，提取今日各 App 用时
│                                 #   - 定期调用 ApiSyncScreenTime() 上传并聚合多端数据
│
├── stats_window.cpp              # 弹出窗口：屏幕时间统计报告
│                                 #   - 七日趋势条形图
│                                 #   - 分类宫格视图、App 列表下钻
│                                 #   - 入口函数：ShowStatsWindow(HWND parent)
│
├── completed_todos_window.cpp    # 弹出窗口：已完成待办列表
│                                 #   - 卡片式列表，支持滚动
│                                 #   - 恢复/彻底删除操作
│                                 #   - 入口函数：ShowCompletedTodosWindow(HWND parent)
│
├── weekly_view_window.h/.cpp     # 弹出窗口：课程表 + 待办周视图
│                                 #   - 周视图网格，支持切换周次
│                                 #   - 从云端拉取或本地缓存加载课程
│                                 #   - 入口函数：ShowWeeklyViewWindow(HWND parent)
│
├── sqlite3.c / sqlite3.h         # SQLite3 嵌入式数据库（Amalgamation，勿修改）
├── json.hpp                      # nlohmann/json 单头文件库（勿修改）
├── resource.h / resources.rc     # Windows 资源文件（应用图标）
├── MiSans-Regular.ttf            # 字体文件（已弃用，现用系统 Microsoft YaHei）
├── update_manifest.json          # 本地版本号记录（仅供参考，实际版本号在 main.cpp 的常量中）
│
├── cmake-build-debug/            # Debug 构建输出（含运行时 INI 配置）
└── cmake-build-release/          # Release 构建输出（含运行时 INI 配置）
    └── math_quiz_lite.ini        # 用户运行时配置文件（与 exe 同目录）
```

---

## 4. 启动流程

```
WinMain()  [main.cpp]
  │
  ├─① SetProcessDPIAware() + 计算 g_Scale
  ├─② Gdiplus::GdiplusStartup()
  ├─③ InitCustomFont()  →  创建 g_MiSansFamily (Microsoft YaHei)
  ├─④ LoadSettings()  →  从 INI 读取所有配置到全局变量
  │
  ├─⑤ ShowLogin(false)  [ui.cpp]
  │     ├─ 读取 INI 中的 email/pass/autoLogin
  │     ├─ [若 autoLogin=true 且有密码] → ExecuteLoginWithRetry() → 成功则跳过登录窗口
  │     ├─ [自动登录失败/未开启] → 显示登录窗口，等待用户手动登录
  │     └─ 登录成功：g_LoginSuccess=true，g_UserId/g_Username 被写入
  │
  ├─⑥ StartTaiReader()  →  启动后台线程，每隔 60s 读一次 Tai SQLite 数据
  │
  ├─⑦ RegisterClassW("MathWidget") + CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST)
  │     → 创建主悬浮窗，WS_EX_LAYERED 启用分层模式
  │
  ├─⑧ std::thread(SyncData).detach()  →  500ms 后首次从云端拉取所有数据
  ├─⑨ CheckForUpdates(false)  →  静默检查 GitHub 版本清单
  │
  └─⑩ 进入 Win32 主消息循环 GetMessage / DispatchMessage
         │
         ├─ WM_TIMER (200ms)  →  RenderWidget()  重绘主窗口
         ├─ WM_USER_REFRESH   →  ResizeWidget()  数据变化后重新计算高度并重绘
         └─ WM_DESTROY        →  PostQuitMessage(0)  退出循环
```

---

## 5. 核心架构：三层模型

```
┌─────────────────────────────────────────────────────┐
│                    UI 层 (ui.cpp)                    │
│  RenderWidget (GDI+ 手绘)  |  各弹窗 WndProc        │
│  负责：绘制、用户交互响应、调用 API 和跳转弹窗       │
└──────────────────────┬──────────────────────────────┘
                       │ 直接读写全局变量
┌──────────────────────▼──────────────────────────────┐
│               状态层 (common.cpp / common.h)         │
│  g_Todos, g_Countdowns, g_UserId, g_BgAlpha ...     │
│  全局变量 + g_DataMutex 互斥锁保护数据集合           │
└──────────┬───────────────────────────────┬──────────┘
           │ API 调用                       │ 读写 INI
┌──────────▼──────────┐         ┌──────────▼──────────┐
│   网络层 (api.cpp)   │         │  持久化层 (utils.cpp) │
│  SendRequest()       │         │  LoadSettings()      │
│  ApiLogin()          │         │  SaveSettings()      │
│  SyncData()          │         │  INI 文件            │
└─────────────────────┘         └─────────────────────┘
```

---

## 6. 开发规范：如何调用 API 接口

### 底层：`SendRequest()`

所有网络请求必须通过 `api.cpp` 中的 `SendRequest` 发起，**禁止在其他文件中直接使用 WinHTTP**。

```cpp
// 函数签名
std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body);

// 自动附加的通用请求头（在 SendRequest 内部硬编码）：
//   Content-Type: application/json
//   Authorization: Bearer {g_AuthToken}   ← 登录后由服务器签发的 HMAC Token
//   （无 Token 时降级为 x-user-id，仅兼容未登录场景如更新检查）

// 返回值约定：
//   成功 → 返回 HTTP 响应 Body 字符串（UTF-8 JSON）
//   失败 → 返回以 "ERROR: " 开头的字符串
```

### 新增一个 API 函数的标准写法

**步骤 1**：在 `api.h` 中声明函数：
```cpp
void ApiDoSomething(const std::wstring &param);
```

**步骤 2**：在 `api.cpp` 中实现，遵循以下模板：
```cpp
void ApiDoSomething(const std::wstring &param) {
    // 1. 构建请求体（统一用 nlohmann/json）
    json j;
    j["user_id"] = g_UserId;
    j["param"]   = ToUtf8(param);
    j["client_updated_at"] = (long long)time(nullptr) * 1000; // 写操作必须带时间戳

    // 2. 发送请求
    std::string res = SendRequest(L"/api/your_endpoint", "POST", j.dump());

    // 3. 错误检查
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"ApiDoSomething 失败: 网络错误");
        return;
    }

    // 4. 解析响应（务必用 try-catch 包裹）
    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            // 成功：更新全局状态
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            // ... 修改 g_Todos 等全局集合 ...
        }
    } catch (...) {
        LogMessage(L"ApiDoSomething 响应解析失败");
    }

    // 5. 🚀 新架构：写操作不再单独发请求，直接修改内存并设 isDirty=true
    //    由下一次 SyncData() 调用时统一通过 /api/sync 批量上传
}
```

**步骤 3**：在 UI 层调用时，**写操作后必须调用 `SyncData()`** 触发 Delta Sync 并重绘：
```cpp
// ui.cpp 中的点击响应示例
ApiDoSomething(param);    // 本地乐观更新，标记 isDirty
SyncData();               // 批量上传脏数据 + 拉取其他设备的增量，发送 WM_USER_REFRESH 重绘
```

### 🚀 Delta Sync 核心流程（`SyncData` 内部逻辑）

```
SyncData()
  ├─ 1. 收集所有 isDirty=true 的 Todo/Countdown，组装变更包
  │      日期字符串 → DateStringToUtcMs() → UTC ms 时间戳
  ├─ 2. 将本机屏幕时间从 g_AppUsage 打包到 screen_time 字段
  ├─ 3. POST /api/sync { user_id, device_id, last_sync_time, todos[], countdowns[], screen_time }
  ├─ 4. 解析响应 { server_todos[], server_countdowns[], new_sync_time, status }
  │      ├─ server_todos: 合并到 g_Todos（uuid 匹配 → 更新，不存在 → 插入，is_deleted → 移除）
  │      │   时间戳 → UtcMsToDateString() → 日期字符串
  │      ├─ server_countdowns: 同上合并到 g_Countdowns
  │      └─ status: 更新 g_UserTier / g_SyncCount / g_SyncLimit
  ├─ 5. 清除本地所有 isDirty 标记，移除 id<0 的软删除条目
  ├─ 6. 持久化 new_sync_time → SaveLastSyncTime()
  └─ 7. GET /api/screen_time 拉取多端聚合屏幕时间 → g_AppUsage
```

### GET 请求（查询）写法
```cpp
std::wstring url = L"/api/resource?user_id=" + std::to_wstring(g_UserId) + L"&date=" + dateStr;
std::string res = SendRequest(url, "GET", ""); // body 传空字符串
```

### 注意事项
- `SendRequest` 是**同步阻塞调用**，若在 UI 线程调用会短暂冻结窗口。长耗时操作必须包在 `std::thread([](){ ... }).detach()` 中
- 所有字符串参数：C++ `std::wstring` → JSON 时必须用 `ToUtf8()` 转换；JSON 响应中的 `std::string` → 界面显示时用 `ToWide()` 转换
- API Host 硬编码在 `common.cpp`：`const std::wstring API_HOST = L"mathquiz.junpgle.me";`，修改域名只改这一处

---

## 7. 开发规范：如何新增一个弹出窗口（页面）

项目中所有"页面"都是独立的 Win32 弹出窗口，遵循以下固定模式：

### 标准模板

**步骤 1**：创建 `my_window.h`：
```cpp
#pragma once
#include <windows.h>

// 唯一的公开入口函数
void ShowMyWindow(HWND parent);
```

**步骤 2**：创建 `my_window.cpp`，遵循此结构：
```cpp
#include "my_window.h"
#include "common.h"
#include "utils.h"
#include "api.h"
using namespace Gdiplus;

// --- 1. 窗口内部状态（用 static 隔离作用域）---
static int s_ScrollY = 0;
static int s_MaxScrollY = 0;

// --- 2. 核心绘制函数 ---
static void DrawMyWindow(Graphics& g, int width, int height) {
    // 字体统一使用 Microsoft YaHei
    FontFamily ff(L"Microsoft YaHei");
    Font titleF(&ff, (REAL)S(22), FontStyleBold, UnitPixel);
    // ... 绘制逻辑 ...
}

// --- 3. 窗口消息处理 ---
static LRESULT CALLBACK MyWindowWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEWHEEL: { /* 滚动处理 */ break; }
        case WM_LBUTTONDOWN: { /* 点击响应 */ break; }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            // 标准双缓冲模板：
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mdc, mbm);
            Graphics graphics(mdc);
            DrawMyWindow(graphics, rc.right, rc.bottom);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old);
            DeleteObject(mbm); DeleteDC(mdc);
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_CLOSE: DestroyWindow(hWnd); break;
        default: return DefWindowProc(hWnd, msg, wp, lp);
    }
    return 0;
}

// --- 4. 公开入口函数（使用 static 防止重复注册 WndClass）---
void ShowMyWindow(HWND parent) {
    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = MyWindowWndProc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MyWindowClass"; // 必须全局唯一
        RegisterClassW(&wc);
        s_registered = true;
    }
    int W = S(600), H = S(500);
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    HWND h = CreateWindowExW(WS_EX_TOPMOST, L"MyWindowClass", L"窗口标题",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, W, H, parent, NULL, GetModuleHandle(NULL), NULL);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
}
```

**步骤 3**：在 `CMakeLists.txt` 的 `add_executable` 中添加新文件：
```cmake
add_executable(MathQuizLite
    ...existing cpp files...
    my_window.cpp
)
```

**步骤 4**：在 `ui.cpp` 中注册入口和点击响应：
```cpp
// ui.cpp 顶部声明
extern void ShowMyWindow(HWND parent);

// WidgetWndProc 中的 WM_RBUTTONUP 菜单添加菜单项
AppendMenuW(hMenu, 0, 1006, L"我的新窗口");

// 处理菜单点击
else if (cmd == 1006) ShowMyWindow(hWnd);
```

### 弹窗与主窗口的区别

| 特性 | 主悬浮窗 (`MathWidget`) | 弹出窗口（stats/completed/weekly等） |
|------|------------------------|--------------------------------------|
| 窗口样式 | `WS_EX_LAYERED \| WS_EX_TOOLWINDOW \| WS_EX_TOPMOST` | `WS_OVERLAPPED \| WS_CAPTION \| WS_SYSMENU` |
| 渲染方式 | `UpdateLayeredWindow`（每 200ms 定时重绘，不走 WM_PAINT） | `WM_PAINT` + 双缓冲 `BitBlt` |
| 透明支持 | 逐像素 ARGB，完全透明 | 不透明（系统标准背景） |
| 任务栏 | 不出现（`WS_EX_TOOLWINDOW`） | 正常显示 |

---

## 8. 开发规范：如何新增全局状态变量

全局变量是项目的"状态管理"，必须严格遵守以下规范：

**步骤 1**：在 `common.h` 中 `extern` 声明：
```cpp
// common.h
extern bool g_MyNewFlag;
extern std::vector<MyStruct> g_MyCollection;
```

**步骤 2**：在 `common.cpp` 中定义并初始化：
```cpp
// common.cpp
bool g_MyNewFlag = false;
std::vector<MyStruct> g_MyCollection;
```

**规则**：
- **只有一处定义**（`common.cpp`），其他文件只 `#include "common.h"` 后直接使用
- **数据集合（`vector`）必须在操作前加锁**：`std::lock_guard<std::recursive_mutex> lock(g_DataMutex);`
- **标量（int/bool/float）** 在 UI 线程内读写不需要加锁，仅跨线程时需要
- 需要持久化的配置，在 `LoadSettings()` 中添加读取，在 `SaveSettings()` 或专用 Save 函数中添加写入

---

## 9. 开发规范：UI 绘制约定

### 颜色与字体使用规范

```cpp
// ✅ 正确：统一使用 Microsoft YaHei
FontFamily ff(L"Microsoft YaHei");
Font titleF(&ff, (REAL)S(22), FontStyleBold,    UnitPixel);  // 标题
Font normalF(&ff, (REAL)S(16), FontStyleRegular, UnitPixel);  // 正文
Font smallF(&ff, (REAL)S(13), FontStyleRegular,  UnitPixel);  // 辅助文字

// ❌ 禁止：不要再使用 MiSans（系统中可能没有该字体）
FontFamily ff(L"MiSans"); // 禁止

// ✅ 正确：所有尺寸必须过 S() 宏进行 DPI 缩放
g.DrawRectangle(&pen, S(15), S(20), S(200), S(30)); // ✅
g.DrawRectangle(&pen, 15, 20, 200, 30);             // ❌ 不适配高 DPI
```

### 主悬浮窗（`RenderWidget`）添加点击区域

主窗口点击检测依赖 `g_HitZones` 列表，每帧绘制时重建：

```cpp
// 1. 在 RenderWidget() 绘制对应元素后，紧接着注册 HitZone
g.DrawString(L"我的按钮", -1, &headF, PointF(x, y), &grBrush);
g_HitZones.push_back({
    Rect(x - S(5), (int)y, S(80), S(20)),  // 点击区域（Gdiplus::Rect）
    item.id,                                 // 数据 ID（无关联传 0）
    MY_TYPE_ID                               // 自定义 type，在 WM_LBUTTONDOWN 中识别
});

// 2. 在 WidgetWndProc 的 WM_LBUTTONDOWN 处理中添加响应
else if (z.type == MY_TYPE_ID) {
    // 处理点击逻辑
}
```

**现有 type ID 分配（勿冲突）：**

| type | 功能 |
|------|------|
| 1 | 新增待办（+按钮） |
| 2 | 新增倒计时（+按钮） |
| 3 | 点击待办条目（切换完成/打开编辑） |
| 4 | 点击倒计时条目（触发删除确认） |
| 5 | 待办删除按钮（[-]） |
| 6 | 屏幕时间标题（打开统计窗口） |
| 7 | 已完成按钮（打开已完成列表） |
| 8 | 日历图标（打开周视图） |
| **9+** | **新功能从 9 开始分配** |

---

## 10. 数据持久化约定

### INI 文件结构（`math_quiz_lite.ini`，与 exe 同目录）

```ini
[Auth]
UserId=1                    # 登录用户 ID
Username=Junpgle            # 显示用户名
Email=xxx@qq.com            # 保存的邮箱（明文）
Pass=<DPAPI十六进制密文>     # 加密后的密码（空=未保存密码）
AutoLogin=1                 # 是否自动登录 (0/1)
SyncInterval=60             # 自动同步间隔（分钟，0=从不）
UserTier=free               # 账户等级（free/pro等）
SyncCount=346               # 今日同步次数（显示用）
SyncLimit=500               # 今日同步上限（显示用）

[Settings]
BgAlpha=200                 # 主窗口背景透明度 (0-255)
TaiDbPath=D:\...\data.db    # Tai 软件数据库路径
TopAppsCount=10             # 屏幕时间显示前N个App
```

### 读写函数对照

| 场景 | 读取函数 | 写入函数 |
|------|---------|---------|
| 启动加载全部配置 | `LoadSettings()` (utils.cpp) | — |
| 登录成功保存账户 | `LoadLoginConfig()` (ui.cpp) | `SaveLoginConfig()` (ui.cpp) |
| 保存账户+密码 | — | `SaveSettings()` (utils.cpp) |
| 保存背景透明度、TopN | — | `SaveAlphaSetting()` (utils.cpp) |
| 保存 Tai 数据库路径 | — | `SaveTaiDbPathSetting()` (utils.cpp) |
| 保存同步限额缓存 | — | `SaveSyncStatusToLocal()` (api.cpp) |

### 本地 JSON 缓存

| 文件 | 内容 | 读写位置 |
|------|------|---------|
| `courses_cache.json` | 课程表数据 | `SaveLocalCourses()` / `LoadLocalCourses()` (api.cpp) |
| `stats_cache.json` | 统计数据缓存 | `stats_window.cpp` 内部 |

---

## 11. 线程安全约定

### 线程结构

```
主线程 (UI线程)
  └─ Win32 消息循环
  └─ RenderWidget() 绘制（每 200ms）
  └─ WidgetWndProc() 响应用户交互

后台线程 1：TaiReader (tai_reader.cpp)
  └─ StartTaiReader() 启动，StopTaiReader() 停止
  └─ 每 60s 读一次 SQLite，写 g_AppUsage，调用 ApiSyncScreenTime

后台线程 2：SyncData (按需 detach)
  └─ 从云端拉取数据，写 g_Todos / g_Countdowns
  └─ 完成后 PostMessage(g_hWidgetWnd, WM_USER_REFRESH, ...) 通知 UI 刷新

后台线程 3：CheckForUpdates (按需 detach)
  └─ 静默检查 GitHub 版本清单，需要更新时弹 MessageBox
```

### 数据访问规则

```cpp
// ✅ 修改数据集合（在任意线程）：必须加锁
{
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    g_Todos.push_back(...);
}

// ✅ 修改完成后通知 UI 线程刷新（从后台线程调用）：
PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0); // 线程安全

// ❌ 禁止从后台线程直接调用 RenderWidget() / ResizeWidget()
// ❌ 禁止从后台线程直接操作 HWND（除 PostMessage 外）
```

---

## 12. 已知技术债与注意事项

### 🔴 安全相关（须知）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 1 | **明文密码常驻内存** | `g_SavedPass` in common.cpp | `wstring` 全程持有解密后的明文，内存转储可读取 |

### 🟡 功能与稳定性

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 3 | **无连接超时** | `SendRequest()` | 仅设了接收超时(10s)，网络挂起时若在UI线程调用则假死 |
| 4 | **`g_AutoLogin` 全局变量未被实际使用** | `common.cpp` | 自动登录逻辑直接从 INI 读取，该变量是冗余状态 |
| 5 | **右键菜单"立即同步"在 UI 线程执行** | `ui.cpp` WidgetWndProc cmd==1001 | `SyncData()` 是同步调用，网络慢时会短暂冻结窗口 |
| 6 | **`recursive_mutex` 掩盖重入问题** | 全局 | 应梳理调用链，理想情况下改为普通 `mutex` |

### ✅ 已修复的历史问题（勿重复修改）

| # | 问题 | 修复时间 |
|---|------|---------|
| 已修 | 登录成功后 `BM_SETCHECK` 误用为读取操作导致自动登录状态永不保存 | 2026-03-07 |
| 已修 | 自动登录失败后 `WM_DESTROY` 触发 `PostQuitMessage` 导致闪退 | 2026-03-07 |
| 已修 | `LoginWndClass` 多次 `ShowLogin` 重复注册 WndClass | 2026-03-07 |
| 已修 | `isLoggingIn` static 变量在多次打开登录窗时状态残留 | 2026-03-07 |
| 已修 | `completed_todos_window.cpp` 字体硬编码为 `MiSans` | 2026-03-07 |
| 已修 | 退出登录后弹窗邮箱框为空（即使 INI 中有保存的邮箱） | 2026-03-07 |
| 已修 | 取消勾选"保存密码"时未从 INI 清除密码字段 | 2026-03-07 |
| 已修 | **鉴权从裸 `x-user-id` 升级为 HMAC Bearer Token**，`SendRequest` 自动携带 `Authorization` 头 | 2026-03-07 |
| 已修 | **同步架构从全量 `GET /api/sync_all` 升级为 Delta Sync `POST /api/sync`**，写操作改为本地乐观更新 + 批量上传 | 2026-03-07 |
| 已修 | **新增 `DeviceId` 持久化**（`[Device]` INI 节），首次运行自动生成，用于多端区分 | 2026-03-07 |
| 已修 | **新增 `LastSyncTime` 持久化**（`[Sync]` INI 节），支持断线重连后的增量拉取 | 2026-03-07 |
| 已修 | **时间字段统一为 UTC ms 时间戳**，`DateStringToUtcMs` / `UtcMsToDateString` / `UtcMsToDateOnly` 工具函数统一转换 | 2026-03-07 |

---

## 13. 构建与编译

### 环境要求

- **编译器**：MinGW-w64（推荐）或 MSVC 2019+
- **构建工具**：CMake 3.10+
- **IDE**：JetBrains CLion（项目配置已适配）
- **目标平台**：Windows x64

### CMake 构建

```bash
# Debug
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug

# Release（用于发布）
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
```

### 链接库清单（`CMakeLists.txt`）

```
winhttp    - HTTP 网络请求
crypt32    - DPAPI 密码加密/解密
gdiplus    - GDI+ 图形渲染
shlwapi    - Shell 路径操作 (PathRemoveFileSpecW 等)
user32 / kernel32 / gdi32 / shell32 / ole32 / advapi32 - Windows 标准库
```

### 编译选项

```cmake
# 隐藏控制台窗口（发布时必须）
target_link_options(MathQuizLite PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
# -static 将 libgcc / libstdc++ 静态链接，发布时无需附带 DLL
```

### 运行时依赖

编译产物 `MathQuizLite.exe` 发布时需同目录包含：
- ~~`MiSans-Regular.ttf`~~ 已改用系统字体，**不再需要**
- `math_quiz_lite.ini`：首次运行会自动创建，无需手动提供

### 版本号管理

版本号在 `main.cpp` 中以整型常量维护：
```cpp
const int CURRENT_VERSION_CODE = 9; // 每次发布递增
```
同时需在 GitHub 仓库的 `update_manifest.json` 中同步更新 `version_code` 字段。

🍅 番茄钟功能需求汇总
1. 基础设置与界面 (UI & Settings)
   专属界面：新增一个独立的番茄钟/专注工作台界面。
   自定义参数：
   单次专注时长。
   单次休息时长。
   专注与休息交替的循环次数。
2. 任务与标签系统 (Tasks & Tags)
   事前绑定：每轮专注开始前，用户需指定一个“待办事项”作为本轮的专注目标。
   自定义多标签：
   支持为任务绑定一个或多个标签（如：工作、阅读、高难度等）。
   标签名称由用户完全自定义。
   云端同步：用户创建的自定义标签需实时同步到云端数据库（D1），保证多端一致。
3. 核心执行与异常处理 (Execution & Resilience)
   中途切换任务：在专注计时中，允许用户提前完成当前任务，并无缝切换至下一个待办事项。系统需记录上一个任务的实际专注时长，并重新开始新任务的计时。
   防误杀/状态恢复机制：
   基于绝对时间戳：不依赖内存计时，开始时计算并向本地存储记录“目标结束时间”。
   后台唤醒与重启：应用被杀或退到后台后重新打开时，通过对比“当前时间”与“目标结束时间”，自动计算并恢复剩余进度。
   超时处理：如果重启时发现倒计时已结束，直接触发“专注结束”的反馈流程。
4. 结束反馈与状态同步 (Completion & Sync)
   完成确认：专注倒计时结束时，主动询问用户“该任务是否已完成？”。
   状态更新：若用户确认完成，将该待办事项的状态标记为“已完成”，并立刻同步至云端数据库。
5. 数据统计与复盘看板 (Statistics & Dashboard)
   基础明细：展示用户的“总专注时长”，并罗列专注期间分别执行了哪些具体任务。
   多维度汇总：
   时间维度：支持按年（Year）、月（Month）、日（Day）进行数据筛选和查看。
   标签维度：支持按用户自定义标签查看专注时长的分布情况（如：了解本月在“学习”标签上投入了多少小时）。
