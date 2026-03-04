#include "api.h"
#include "utils.h"
#include "common.h"
#include <winhttp.h>
#include <ctime>
#include <debugapi.h>
#include <iostream>
#include <shlwapi.h> // 🚀 引入 SHLWAPI 以便写入文件

#pragma comment(lib, "shlwapi.lib")

using json = nlohmann::json;

bool ApiFetchUserStatus();

/**
 * 内部辅助：统一日志输出
 */
void LogMessage(const std::wstring &msg) {
    OutputDebugStringW(L"\n[MathQuiz] ");
    OutputDebugStringW(msg.c_str());

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode;
        if (GetConsoleMode(hOut, &dwMode)) {
            std::wstring fullMsg = L"[API LOG] " + msg + L"\n";
            WriteConsoleW(hOut, fullMsg.c_str(), (DWORD) fullMsg.length(), NULL, NULL);
            return;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "[API LOG] " << ToUtf8(msg) << std::endl;
    std::cout.flush();
}

/**
 * 🚀 内部辅助：保存同步状态到本地 INI 文件
 */
void SaveSyncStatusToLocal() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WritePrivateProfileStringW(L"Auth", L"UserTier", g_UserTier.c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"SyncCount", std::to_wstring(g_SyncCount).c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"SyncLimit", std::to_wstring(g_SyncLimit).c_str(), path);
}

/**
 * 🚀 核心新增：将内存中的课程表保存至本地 JSON
 */
void SaveLocalCourses() {
    try {
        json jArr = json::array(); {
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            for (const auto &c: g_Courses) {
                json j;
                j["id"] = c.id;
                j["course_name"] = ToUtf8(c.courseName);
                j["room_name"] = ToUtf8(c.roomName);
                j["teacher_name"] = ToUtf8(c.teacherName);
                j["start_time"] = c.startTime;
                j["end_time"] = c.endTime;
                j["weekday"] = c.weekday;
                j["week_index"] = c.weekIndex;
                j["lesson_type"] = ToUtf8(c.lessonType);
                jArr.push_back(j);
            }
        }
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        PathRemoveFileSpecW(path);
        PathAppendW(path, L"courses_cache.json");

        HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string s = jArr.dump();
            DWORD written;
            WriteFile(hFile, s.c_str(), (DWORD) s.length(), &written, NULL);
            CloseHandle(hFile);
            LogMessage(L"已成功将云端课表缓存至本地!");
        }
    } catch (...) {
        LogMessage(L"保存本地课表缓存失败");
    }
}

/**
 * 🚀 核心新增：从本地 JSON 读取课程表
 */
void LoadLocalCourses() {
    try {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        PathRemoveFileSpecW(path);
        PathAppendW(path, L"courses_cache.json");

        HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(hFile, NULL);
            if (size > 0) {
                std::string s(size, '\0');
                DWORD read;
                ReadFile(hFile, &s[0], size, &read, NULL);

                auto jArr = json::parse(s);
                if (jArr.is_array()) {
                    std::vector<Course> temp;
                    for (auto &it: jArr) {
                        Course c;
                        c.id = it.value("id", 0);
                        c.courseName = ToWide(it.value("course_name", ""));
                        c.roomName = ToWide(it.value("room_name", ""));
                        c.teacherName = ToWide(it.value("teacher_name", ""));
                        c.startTime = it.value("start_time", 0);
                        c.endTime = it.value("end_time", 0);
                        c.weekday = it.value("weekday", 1);
                        c.weekIndex = it.value("week_index", 1);
                        c.lessonType = ToWide(it.value("lesson_type", ""));
                        temp.push_back(c);
                    }
                    std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                    g_Courses = temp;
                    LogMessage(L"成功从本地缓存加载了 " + std::to_wstring(temp.size()) + L" 节课程");
                }
            }
            CloseHandle(hFile);
        } else {
            LogMessage(L"未发现本地课表缓存文件");
        }
    } catch (...) {
        LogMessage(L"解析本地课表缓存失败");
    }
}

std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response = "";
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "ERROR: Session failed";

    HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "ERROR: Connect failed";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "ERROR: Request failed";
    }

    DWORD timeout = 10000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (g_UserId > 0) headers += L"x-user-id: " + std::to_wstring(g_UserId) + L"\r\n";

    if (WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID) body.c_str(), (DWORD) body.length(),
                           (DWORD) body.length(), 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;
                char *pszOutBuffer = new char[dwSize + 1];
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, (LPVOID) pszOutBuffer, dwSize, &dwDownloaded)) {
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
            if (resp["user"].contains("tier")) g_UserTier = ToWide(resp["user"]["tier"].get<std::string>());
            ApiFetchUserStatus();
            return "SUCCESS";
        }
        if (resp.contains("error")) return resp["error"].get<std::string>();
    } catch (...) {
    }
    return "FAILED";
}

bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    return ApiLogin(g_SavedEmail, g_SavedPass) == "SUCCESS";
}

void ApiAddTodo(const std::wstring &content, const std::wstring &createdDate, const std::wstring &dueDate,
                bool isDone) {
    json j;
    j["user_id"] = g_UserId;
    j["content"] = ToUtf8(content);
    j["created_date"] = ToUtf8(createdDate);
    j["due_date"] = ToUtf8(dueDate);
    j["is_completed"] = isDone;
    j["client_updated_at"] = (long long) time(nullptr) * 1000;
    SendRequest(L"/api/todos", "POST", j.dump());
    ApiFetchUserStatus();
}

void ApiToggleTodo(int id, bool done) {
    json j;
    j["id"] = id;
    j["is_completed"] = done;
    j["client_updated_at"] = (long long) time(nullptr) * 1000;
    SendRequest(L"/api/todos/toggle", "POST", j.dump());
    ApiFetchUserStatus();
}

void ApiDeleteTodo(int id) {
    json j;
    j["id"] = id;
    j["client_updated_at"] = (long long) time(nullptr) * 1000;
    SendRequest(L"/api/todos", "DELETE", j.dump());
    ApiFetchUserStatus();
}

void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    json j;
    j["user_id"] = g_UserId;
    j["title"] = ToUtf8(title);
    j["target_time"] = ToUtf8(dateStr);
    j["client_updated_at"] = (long long) time(nullptr) * 1000;
    SendRequest(L"/api/countdowns", "POST", j.dump());
    ApiFetchUserStatus();
}

void ApiDeleteCountdown(int id) {
    json j;
    j["id"] = id;
    j["client_updated_at"] = (long long) time(nullptr) * 1000;
    SendRequest(L"/api/countdowns", "DELETE", j.dump());
    ApiFetchUserStatus();
}

std::map<std::wstring, int> ApiSyncScreenTime(const std::map<std::wstring, int> &localData, const std::wstring &dateStr,
                                              const std::wstring &deviceName) {
    if (g_UserId <= 0) return localData;
    json payload;
    payload["user_id"] = g_UserId;
    payload["device_name"] = ToUtf8(deviceName);
    payload["record_date"] = ToUtf8(dateStr);
    json apps = json::array();
    for (const auto &p: localData) {
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
            for (const auto &item: jArr) {
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
    } catch (...) {
    }
    return agg.empty() ? localData : agg;
}

bool ApiFetchUserStatus() {
    if (g_UserId <= 0) return false;
    LogMessage(L"开始限额同步...");
    std::wstring statusUrl = L"/api/user/status?user_id=" + std::to_wstring(g_UserId);
    std::string res = SendRequest(statusUrl, "GET", "");
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"限额状态拉取失败 (离线)");
        return false;
    }
    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            if (resp.contains("tier")) g_UserTier = ToWide(resp["tier"].get<std::string>());
            if (resp.contains("sync_count")) g_SyncCount = resp["sync_count"].get<int>();
            if (resp.contains("sync_limit")) g_SyncLimit = resp["sync_limit"].get<int>();
            SaveSyncStatusToLocal();
            if (g_hWidgetWnd)
                PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
            return true;
        }
    } catch (...) { LogMessage(L"限额状态解析 JSON 异常"); }
    return false;
}

void SyncData() {
    if (g_UserId <= 0) return;
    std::wstring freqLog = (g_SyncInterval > 0)
                               ? L"执行定时自动同步 (周期: " + std::to_wstring(g_SyncInterval) + L" 分钟)..."
                               : L"执行手动/初始化同步...";
    LogMessage(freqLog);
    std::wstring syncUrl = L"/api/sync_all?user_id=" + std::to_wstring(g_UserId);
    std::string res = SendRequest(syncUrl, "GET", "");
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"聚合同步失败: 网络错误");
        return;
    }
    try {
        auto resp = json::parse(res);
        if (!resp.contains("success") || !resp["success"].get<bool>()) {
            LogMessage(L"聚合同步失败: 服务器业务异常");
            return;
        }
        if (resp.contains("tier")) g_UserTier = ToWide(resp["tier"].get<std::string>());
        if (resp.contains("sync_count")) g_SyncCount = resp["sync_count"].get<int>();
        if (resp.contains("sync_limit")) g_SyncLimit = resp["sync_limit"].get<int>();
        SaveSyncStatusToLocal();
        auto data = resp["data"];
        int todoCount = 0;
        int countdownCount = 0;

        if (data.contains("todos") && data["todos"].is_array()) {
            std::vector<Todo> tempTodos;
            for (auto &it: data["todos"]) {
                bool isDeleted = false;
                if (it.contains("is_deleted")) {
                    auto val = it["is_deleted"];
                    isDeleted = val.is_number() ? (val.get<int>() != 0) : val.get<bool>();
                }
                if (isDeleted) continue;
                tempTodos.push_back({
                    it["id"].get<int>(), ToWide(it["content"].get<std::string>()),
                    it["is_completed"].is_number()
                        ? (it["is_completed"].get<int>() != 0)
                        : it["is_completed"].get<bool>(),
                    0,
                    it.contains("created_date") && !it["created_date"].is_null()
                        ? ToWide(it["created_date"].get<std::string>())
                        : L"",
                    it.contains("due_date") && !it["due_date"].is_null()
                        ? ToWide(it["due_date"].get<std::string>())
                        : L""
                });
            }
            todoCount = (int) tempTodos.size();
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            g_Todos = tempTodos;
        }

        if (data.contains("countdowns") && data["countdowns"].is_array()) {
            std::vector<Countdown> tempCds;
            for (auto &it: data["countdowns"]) {
                bool isDeleted = false;
                if (it.contains("is_deleted")) {
                    auto val = it["is_deleted"];
                    isDeleted = val.is_number() ? (val.get<int>() != 0) : val.get<bool>();
                }
                if (isDeleted) continue;
                std::wstring ds = ToWide(it["target_time"].get<std::string>());
                tempCds.push_back({
                    it["id"].get<int>(), ToWide(it["title"].get<std::string>()), ds, CalculateDaysLeft(ds), 0
                });
            }
            countdownCount = (int) tempCds.size();
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            g_Countdowns = tempCds;
        }
        std::wstring resultMsg = L"聚合同步完成! (待办: " + std::to_wstring(todoCount) + L", 倒计时: " +
                                 std::to_wstring(countdownCount) + L")";
        LogMessage(resultMsg);
        if (g_hWidgetWnd)
            PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
    } catch (...) { LogMessage(L"聚合同步数据解析失败"); }
}

void ApiFetchCourses() {
    if (g_UserId <= 0) return;
    LogMessage(L"开始从云端拉取最新课程表...");
    std::wstring url = L"/api/courses?user_id=" + std::to_wstring(g_UserId);
    std::string res = SendRequest(url, "GET", "");

    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"课程表拉取失败: 网络异常");
        return;
    }

    try {
        auto jArr = json::parse(res);
        if (jArr.is_array()) {
            std::vector<Course> tempCourses;
            for (auto &it: jArr) {
                Course c;
                c.id = it.value("id", 0);
                c.courseName = ToWide(it.value("course_name", "未知课程"));
                c.roomName = ToWide(it.value("room_name", "未知教室"));
                c.teacherName = ToWide(it.value("teacher_name", ""));
                c.startTime = it.value("start_time", 0);
                c.endTime = it.value("end_time", 0);
                c.weekday = it.value("weekday", 1);
                c.weekIndex = it.value("week_index", 1);
                if (it.contains("lesson_type") && !it["lesson_type"].is_null()) {
                    c.lessonType = ToWide(it.value("lesson_type", ""));
                }
                tempCourses.push_back(c);
            } {
                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                g_Courses = tempCourses;
            }

            // 🚀 拉取成功后立刻写入本地文件持久化缓存
            SaveLocalCourses();

            LogMessage(L"课程表拉取并缓存成功! 共 " + std::to_wstring(tempCourses.size()) + L" 节课");
            if (g_hWidgetWnd)
                PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
        } else if (jArr.is_object() && jArr.contains("error")) {
            LogMessage(L"课程表拉取被拒绝: " + ToWide(jArr["error"].get<std::string>()));
        }
    } catch (...) {
        LogMessage(L"课程表数据解析 JSON 失败");
    }
}
