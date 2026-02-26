#include "api.h"
#include "utils.h"
#include <winhttp.h>

using json = nlohmann::json;

// 内部辅助函数：发送网络请求
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

    // 设置超时（5秒）
    DWORD timeout = 5000;
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

// 登录接口
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
            return "SUCCESS";
        }
        if (resp.contains("error")) return resp["error"].get<std::string>();
    } catch(...) {}
    return "FAILED";
}

// 自动登录尝试
bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    return ApiLogin(g_SavedEmail, g_SavedPass) == "SUCCESS";
}

// 待办事项操作
void ApiAddTodo(const std::wstring &content) {
    json j;
    j["user_id"] = g_UserId;
    j["content"] = ToUtf8(content);
    SendRequest(L"/api/todos", "POST", j.dump());
}

void ApiToggleTodo(int id, bool done) {
    json j;
    j["id"] = id;
    j["is_completed"] = done;
    SendRequest(L"/api/todos/toggle", "POST", j.dump());
}

void ApiDeleteTodo(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/todos", "DELETE", j.dump());
}

// 倒计时操作
void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    json j;
    j["user_id"] = g_UserId;
    j["title"] = ToUtf8(title);
    j["target_time"] = ToUtf8(dateStr);
    SendRequest(L"/api/countdowns", "POST", j.dump());
}

void ApiDeleteCountdown(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/countdowns", "DELETE", j.dump());
}

// 屏幕时间多设备同步
std::map<std::wstring, int> ApiSyncScreenTime(const std::map<std::wstring, int>& localData, const std::wstring& dateStr, const std::wstring& deviceName) {
    if (g_UserId <= 0) return localData;

    // 1. 上报本地数据
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

    // 2. 拉取云端聚合数据
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

                // 为了逻辑层的增量计算，返回一个纯 App 聚合 map
                agg[r.appName] += r.seconds;
            }
            // 更新全局明细供 UI 绘制图标
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            g_AppUsage = details;
        }
    } catch(...) {}

    return agg.empty() ? localData : agg;
}

// 全局同步任务
void SyncData() {
    if (g_UserId <= 0) return;

    // 1. 同步待办事项
    std::string resTodos = SendRequest(L"/api/todos?user_id=" + std::to_wstring(g_UserId), "GET", "");
    if (!resTodos.empty() && resTodos.find("ERROR") != 0) {
        try {
            auto j = json::parse(resTodos);
            std::vector<Todo> temp;
            for (auto &it : j) {
                temp.push_back({
                    it["id"].get<int>(),
                    ToWide(it["content"].get<std::string>()),
                    it["is_completed"].get<int>() != 0,
                    0
                });
            }
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            g_Todos = temp;
        } catch (...) {}
    }

    // 2. 同步倒计时
    std::string resCounts = SendRequest(L"/api/countdowns?user_id=" + std::to_wstring(g_UserId), "GET", "");
    if (!resCounts.empty() && resCounts.find("ERROR") != 0) {
        try {
            auto j = json::parse(resCounts);
            std::vector<Countdown> temp;
            for (auto &it : j) {
                std::wstring ds = ToWide(it["target_time"].get<std::string>());
                temp.push_back({
                    it["id"].get<int>(),
                    ToWide(it["title"].get<std::string>()),
                    ds,
                    CalculateDaysLeft(ds),
                    0
                });
            }
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            g_Countdowns = temp;
        } catch (...) {}
    }

    // 通知 UI 刷新
    if (g_hWidgetWnd) {
        PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
    }
}