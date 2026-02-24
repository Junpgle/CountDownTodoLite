#include "api.h"
#include "utils.h"

using json = nlohmann::json;

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef SECURITY_FLAG_IGNORE_UNKNOWN_CA
#define SECURITY_FLAG_IGNORE_UNKNOWN_CA 0x00000100
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
#define SECURITY_FLAG_IGNORE_CERT_DATE_INVALID 0x00002000
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_CN_INVALID
#define SECURITY_FLAG_IGNORE_CERT_CN_INVALID 0x00001000
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE
#define SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE 0x00000200
#endif

std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        int timeout = 15000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(), NULL,
                                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

                std::wstring headers = L"Content-Type: application/json\r\n";
                if (WinHttpSendRequest(hRequest, headers.c_str(), -1L, (LPVOID) body.c_str(), (DWORD) body.size(), (DWORD) body.size(), 0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        DWORD dwSize = 0, dwDownloaded = 0;
                        do {
                            dwSize = 0;
                            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                            if (dwSize == 0) break;
                            std::vector<char> buffer(dwSize + 1);
                            if (WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded)) response.append(buffer.data(), dwDownloaded);
                        } while (dwSize > 0);
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }
    return response;
}

void ApiToggleTodo(int id, bool currentStatus) {
    json j;
    j["id"] = id;
    j["is_completed"] = !currentStatus;
    SendRequest(L"/api/todos/toggle", "POST", j.dump());
}

void ApiAddTodo(const std::wstring &content) {
    json j;
    j["user_id"] = g_UserId;
    j["content"] = ToUtf8(content);
    SendRequest(L"/api/todos", "POST", j.dump());
}

void ApiDeleteTodo(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/todos", "DELETE", j.dump());
}

void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    std::wstring isoTime = dateStr + L"T00:00:00Z";
    json j;
    j["user_id"] = g_UserId;
    j["title"] = ToUtf8(title);
    j["target_time"] = ToUtf8(isoTime);
    SendRequest(L"/api/countdowns", "POST", j.dump());
}

void ApiDeleteCountdown(int id) {
    json j;
    j["id"] = id;
    SendRequest(L"/api/countdowns", "DELETE", j.dump());
}

std::map<std::wstring, int> ApiSyncScreenTime(const std::map<std::wstring, int>& localData, const std::wstring& dateStr, const std::wstring& deviceName) {
    if (g_UserId == 0) return localData;

    // 1. 上报本地设备今天的数据
    json payload;
    payload["user_id"] = g_UserId;
    payload["device_name"] = ToUtf8(deviceName);
    payload["record_date"] = ToUtf8(dateStr);

    json apps = json::array();
    for (const auto& pair : localData) {
        json app;
        app["app_name"] = ToUtf8(pair.first);
        app["duration"] = pair.second;
        apps.push_back(app);
    }
    payload["apps"] = apps;

    SendRequest(L"/api/screen_time", "POST", payload.dump());

    // 2. 拉取服务器端按应用聚合后的总数据 (包含所有设备)
    // 修正点：getUrl 应该使用 std::wstring 构建，且不再对 dateStr 进行 ToUtf8 转换以避免类型冲突
    std::wstring getUrl = L"/api/screen_time?user_id=" + std::to_wstring(g_UserId) + L"&date=" + dateStr;
    std::string resGet = SendRequest(getUrl, "GET", "");

    std::map<std::wstring, int> aggregatedData = localData;
    if (!resGet.empty() && resGet.find("ERROR") != 0) {
        try {
            auto j = json::parse(resGet);
            if (j.is_array()) {
                aggregatedData.clear();
                for (const auto& item : j) {
                    std::wstring appName = ToWide(item["app_name"]);
                    int duration = item["duration"];
                    aggregatedData[appName] = duration;
                }
            }
        } catch (...) {}
    }
    return aggregatedData;
}

void SyncData() {
    if (g_UserId == 0) return;
    std::wstring ts = std::to_wstring(time(NULL));

    // 同步待办
    std::string todoJson = SendRequest(L"/api/todos?user_id=" + std::to_wstring(g_UserId) + L"&t=" + ts, "GET", "");
    if (!todoJson.empty()) {
        try {
            auto j = json::parse(todoJson);
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            std::vector<Todo> mergedTodos;
            std::map<int, Todo> localMap;
            for (const auto &t: g_Todos) if (t.id > 0) localMap[t.id] = t;

            for (const auto &item: j) {
                int id = item["id"].get<int>();
                std::string contentUtf8 = item["content"];
                bool cloudDone = item["is_completed"].is_boolean() ? item["is_completed"].get<bool>() : (item["is_completed"].is_number() && item["is_completed"].get<int>() == 1);
                std::string timeStr = item.contains("updated_at") && !item["updated_at"].is_null() ? item["updated_at"] : item["created_at"];
                time_t cloudTime = ParseSqlTime(timeStr);

                Todo newItem = {id, ToWide(contentUtf8), cloudDone, cloudTime};
                if (localMap.count(id) && localMap[id].lastUpdated > cloudTime) {
                    newItem.isDone = localMap[id].isDone;
                    newItem.lastUpdated = localMap[id].lastUpdated;
                    bool statusToPush = localMap[id].isDone;
                    CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
                        auto *pData = (std::pair<int, bool> *) p;
                        ApiToggleTodo(pData->first, !pData->second);
                        delete pData;
                        return 0;
                    }, new std::pair<int, bool>(id, statusToPush), 0, NULL);
                }
                mergedTodos.push_back(newItem);
            }
            g_Todos = mergedTodos;
        } catch (...) {}
    }

    // 同步倒计时
    std::string countJson = SendRequest(L"/api/countdowns?user_id=" + std::to_wstring(g_UserId) + L"&t=" + ts, "GET", "");
    if (!countJson.empty()) {
        try {
            auto j = json::parse(countJson);
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            g_Countdowns.clear();
            for (const auto &item: j) {
                std::string dateRaw = item.contains("target_time") ? item["target_time"] : item["date"];
                std::string timeStr = item.contains("updated_at") && !item["updated_at"].is_null() ? item["updated_at"] : item["created_at"];
                std::wstring dateW = ToWide(dateRaw);
                g_Countdowns.push_back({item["id"].get<int>(), ToWide(item["title"]), dateW.substr(0, 10), CalculateDaysLeft(dateW.substr(0, 10)), ParseSqlTime(timeStr)});
            }
        } catch (...) {}
    }

    if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
}

bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    std::string body = "{\"email\":\"" + ToUtf8(g_SavedEmail) + "\",\"password\":\"" + ToUtf8(g_SavedPass) + "\"}";
    std::string res = SendRequest(L"/api/auth/login", "POST", body);
    if (res.empty()) return false;
    try {
        auto j = json::parse(res);
        if (j["success"].get<bool>()) {
            g_UserId = j["user"]["id"];
            g_Username = ToWide(j["user"]["username"]);
            SaveSettings(g_UserId, g_Username, g_SavedEmail, g_SavedPass, true);
            return true;
        }
    } catch (...) {}
    return false;
}