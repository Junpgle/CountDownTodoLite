#include "api.h"
#include "utils.h"
#include "common.h"
#include <winhttp.h>
#include <ctime>
#include <debugapi.h>
#include <iostream>

using json = nlohmann::json;

// 前瞻声明
void ApiFetchUserStatus();

/**
 * 🚀 内部辅助：统一日志输出 (防乱码增强版)
 * 解决 Windows GUI 程序在 CLion/CMD 中中文显示为 "寮€濮嬫墽琛" 的问题
 */
void LogMessage(const std::wstring& msg) {
    // 1. 发送到 Windows 调试器 (在 CLion 的 Debug -> Debugger 标签页可见)
    OutputDebugStringW(L"\n[MathQuiz] ");
    OutputDebugStringW(msg.c_str());

    // 2. 尝试使用 Windows 原生 Unicode 控制台输出 (最稳妥的防乱码方案)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode;
        if (GetConsoleMode(hOut, &dwMode)) {
            // 如果是真实的控制台窗口，直接写入宽字符
            std::wstring fullMsg = L"[API LOG] " + msg + L"\n";
            WriteConsoleW(hOut, fullMsg.c_str(), (DWORD)fullMsg.length(), NULL, NULL);
            return;
        }
    }

    // 3. 兜底方案：如果是重定向管道（如某些 IDE 运行环境）
    // 强制设置输出编码为 UTF-8 并通过 std::cout 输出
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "[API LOG] " << ToUtf8(msg) << std::endl;
    std::cout.flush();
}

/**
 * 内部辅助函数：发送网络请求
 */
std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response = "";
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "ERROR: Session failed";

    HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "ERROR: Connect failed";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "ERROR: Request failed";
    }

    // 设置超时（10秒）
    DWORD timeout = 10000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (g_UserId > 0) {
        headers += L"x-user-id: " + std::to_wstring(g_UserId) + L"\r\n";
    }

    if (WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;
                char* pszOutBuffer = new char[dwSize + 1];
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                    pszOutBuffer[dwDownloaded] = '\0';
                    response += pszOutBuffer;
                }
                delete[] pszOutBuffer;
            } while (dwSize > 0);
        }
    } else {
        response = "ERROR: Network Error";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

/**
 * 登录接口
 */
std::string ApiLogin(const std::wstring &email, const std::wstring &password) {
    json j;
    j["email"] = ToUtf8(email);
    j["password"] = ToUtf8(password);

    std::string res = SendRequest(L"/api/auth/login", "POST", j.dump());
    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            g_UserId = resp["user"]["id"].get<int>();
            g_Username = ToWide(resp["user"]["username"].get<std::string>());

            if (resp["user"].contains("tier")) {
                g_UserTier = ToWide(resp["user"]["tier"].get<std::string>());
            }
            return "SUCCESS";
        }
        if (resp.contains("error")) return resp["error"].get<std::string>();
    } catch(...) {}
    return "FAILED";
}

/**
 * 自动登录尝试
 */
bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    return ApiLogin(g_SavedEmail, g_SavedPass) == "SUCCESS";
}

/**
 * 待办事项操作
 */
void ApiAddTodo(const std::wstring &content, const std::wstring &createdDate, const std::wstring &dueDate, bool isDone) {
    json j;
    j["user_id"] = g_UserId;
    j["content"] = ToUtf8(content);
    j["created_date"] = ToUtf8(createdDate);
    j["due_date"] = ToUtf8(dueDate);
    j["is_completed"] = isDone;
    j["client_updated_at"] = (long long)time(nullptr) * 1000;
    SendRequest(L"/api/todos", "POST", j.dump());
    ApiFetchUserStatus();
}

void ApiToggleTodo(int id, bool done) {
    json j;
    j["id"] = id;
    j["is_completed"] = done;
    j["client_updated_at"] = (long long)time(nullptr) * 1000;
    SendRequest(L"/api/todos/toggle", "POST", j.dump());
    ApiFetchUserStatus();
}

void ApiDeleteTodo(int id) {
    json j;
    j["id"] = id;
    j["client_updated_at"] = (long long)time(nullptr) * 1000;
    SendRequest(L"/api/todos", "DELETE", j.dump());
    ApiFetchUserStatus();
}

/**
 * 倒计时操作
 */
void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    json j;
    j["user_id"] = g_UserId;
    j["title"] = ToUtf8(title);
    j["target_time"] = ToUtf8(dateStr);
    j["client_updated_at"] = (long long)time(nullptr) * 1000;
    SendRequest(L"/api/countdowns", "POST", j.dump());
    ApiFetchUserStatus();
}

void ApiDeleteCountdown(int id) {
    json j;
    j["id"] = id;
    j["client_updated_at"] = (long long)time(nullptr) * 1000;
    SendRequest(L"/api/countdowns", "DELETE", j.dump());
    ApiFetchUserStatus();
}

/**
 * 屏幕时间多设备同步
 */
std::map<std::wstring, int> ApiSyncScreenTime(const std::map<std::wstring, int>& localData, const std::wstring& dateStr, const std::wstring& deviceName) {
    if (g_UserId <= 0) return localData;

    json payload;
    payload["user_id"] = g_UserId;
    payload["device_name"] = ToUtf8(deviceName);
    payload["record_date"] = ToUtf8(dateStr);

    json apps = json::array();
    for (const auto& p : localData) {
        json a;
        a["app_name"] = ToUtf8(p.first);
        a["duration"] = p.second;
        apps.push_back(a);
    }
    payload["apps"] = apps;
    SendRequest(L"/api/screen_time", "POST", payload.dump());

    std::wstring getUrl = L"/api/screen_time?user_id=" + std::to_wstring(g_UserId) + L"&date=" + dateStr;
    std::string res = SendRequest(getUrl, "GET", "");

    std::map<std::wstring, int> agg;
    try {
        auto jArr = json::parse(res);
        if (jArr.is_array()) {
            std::vector<AppUsageRecord> details;
            for (const auto& item : jArr) {
                AppUsageRecord r;
                r.appName = ToWide(item["app_name"].get<std::string>());
                r.deviceName = ToWide(item["device_name"].get<std::string>());
                r.seconds = item["duration"].get<int>();
                details.push_back(r);
                agg[r.appName] += r.seconds;
            }
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            g_AppUsage = details;
        }
    } catch(...) {}

    return agg.empty() ? localData : agg;
}

/**
 * 🚀 核心改进：从云端获取限额状态
 */
void ApiFetchUserStatus() {
    if (g_UserId <= 0) return;

    LogMessage(L"开始限额同步...");

    std::wstring statusUrl = L"/api/user/status?user_id=" + std::to_wstring(g_UserId);
    std::string res = SendRequest(statusUrl, "GET", "");

    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"限额同步失败: 网络异常");
        return;
    }

    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            if (resp.contains("tier")) g_UserTier = ToWide(resp["tier"].get<std::string>());
            if (resp.contains("sync_count")) g_SyncCount = resp["sync_count"].get<int>();
            if (resp.contains("sync_limit")) g_SyncLimit = resp["sync_limit"].get<int>();

            std::wstring debugMsg = L"限额更新成功: 等级=" + g_UserTier +
                                   L", 已用=" + std::to_wstring(g_SyncCount) + L"/" + std::to_wstring(g_SyncLimit);
            LogMessage(debugMsg);

            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
        }
    } catch (...) {
        LogMessage(L"限额数据解析 JSON 失败");
    }
}

/**
 * 全局同步任务 (聚合接口)
 */
void SyncData() {
    if (g_UserId <= 0) return;

    LogMessage(L"开始执行全量聚合同步...");

    std::wstring syncUrl = L"/api/sync_all?user_id=" + std::to_wstring(g_UserId);
    std::string res = SendRequest(syncUrl, "GET", "");

    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"全量同步失败: 无法连接服务器");
        return;
    }

    try {
        auto resp = json::parse(res);
        if (!resp.contains("success") || !resp["success"].get<bool>()) {
            LogMessage(L"全量同步失败: 后端逻辑错误");
            return;
        }

        if (resp.contains("tier")) g_UserTier = ToWide(resp["tier"].get<std::string>());
        if (resp.contains("sync_count")) g_SyncCount = resp["sync_count"].get<int>();
        if (resp.contains("sync_limit")) g_SyncLimit = resp["sync_limit"].get<int>();

        auto data = resp["data"];
        int todoCount = 0;
        int countdownCount = 0;

        // 解析待办事项
        if (data.contains("todos") && data["todos"].is_array()) {
            std::vector<Todo> tempTodos;
            for (auto &it : data["todos"]) {
                bool isDeleted = false;
                if (it.contains("is_deleted")) {
                    auto val = it["is_deleted"];
                    isDeleted = val.is_number() ? (val.get<int>() != 0) : val.get<bool>();
                }
                if (isDeleted) continue;

                tempTodos.push_back({
                    it["id"].get<int>(),
                    ToWide(it["content"].get<std::string>()),
                    it["is_completed"].is_number() ? (it["is_completed"].get<int>() != 0) : it["is_completed"].get<bool>(),
                    0,
                    it.contains("created_date") && !it["created_date"].is_null() ? ToWide(it["created_date"].get<std::string>()) : L"",
                    it.contains("due_date") && !it["due_date"].is_null() ? ToWide(it["due_date"].get<std::string>()) : L""
                });
            }
            todoCount = (int)tempTodos.size();
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            g_Todos = tempTodos;
        }

        // 解析倒计时
        if (data.contains("countdowns") && data["countdowns"].is_array()) {
            std::vector<Countdown> tempCds;
            for (auto &it : data["countdowns"]) {
                bool isDeleted = false;
                if (it.contains("is_deleted")) {
                    auto val = it["is_deleted"];
                    isDeleted = val.is_number() ? (val.get<int>() != 0) : val.get<bool>();
                }
                if (isDeleted) continue;

                std::wstring ds = ToWide(it["target_time"].get<std::string>());
                tempCds.push_back({
                    it["id"].get<int>(),
                    ToWide(it["title"].get<std::string>()),
                    ds,
                    CalculateDaysLeft(ds),
                    0
                });
            }
            countdownCount = (int)tempCds.size();
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            g_Countdowns = tempCds;
        }

        std::wstring resultMsg = L"全量同步成功! 获取待办 " + std::to_wstring(todoCount) +
                                L" 条, 倒计时 " + std::to_wstring(countdownCount) + L" 条";
        LogMessage(resultMsg);

        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);

    } catch (...) {
        LogMessage(L"全量同步解析错误");
    }
}