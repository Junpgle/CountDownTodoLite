#include "api.h"
#include "utils.h"
#include "common.h"
#include <winhttp.h>
#include <ctime>
#include <debugapi.h>
#include <iostream>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

using json = nlohmann::json;

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

// ============================================================
// 🚀 本地数据缓存：Todos + Countdowns 持久化到 data_cache.json
// 目的：支持 Delta Sync 重启后恢复本地数据，无需等待网络即可显示
// ============================================================

// 内部辅助：获取 data_cache.json 的完整路径
static std::wstring GetDataCachePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"data_cache.json");
    return path;
}

void SaveLocalData() {
    try {
        json root;
        root["version"]    = 2; // 格式版本号，未来升级格式时用于兼容判断
        root["saved_at"]   = (long long)time(nullptr) * 1000LL;
        root["user_id"]    = g_UserId;

        json todosArr = json::array();
        json cdsArr   = json::array();

        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

            for (const auto &t : g_Todos) {
                json j;
                j["id"]           = t.id;
                j["uuid"]         = ToUtf8(t.uuid);
                j["content"]      = ToUtf8(t.content);
                j["is_done"]      = t.isDone;
                j["is_dirty"]     = t.isDirty;
                j["last_updated"] = (long long)t.lastUpdated * 1000LL;
                // 日期字段：存为 UTC ms 时间戳（0 表示空）
                j["created_date_ms"] = DateStringToUtcMs(t.createdDate);
                j["due_date_ms"]     = DateStringToUtcMs(t.dueDate);
                todosArr.push_back(j);
            }

            for (const auto &c : g_Countdowns) {
                json j;
                j["id"]           = c.id;
                j["uuid"]         = ToUtf8(c.uuid);
                j["title"]        = ToUtf8(c.title);
                j["is_dirty"]     = c.isDirty;
                j["last_updated"] = (long long)c.lastUpdated * 1000LL;
                // target_time 存为 UTC ms
                j["target_time_ms"] = DateStringToUtcMs(c.dateStr);
                cdsArr.push_back(j);
            }
        }

        root["todos"]      = todosArr;
        root["countdowns"] = cdsArr;

        std::wstring cachePath = GetDataCachePath();
        HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string s = root.dump();
            DWORD written;
            WriteFile(hFile, s.c_str(), (DWORD)s.length(), &written, NULL);
            CloseHandle(hFile);
            LogMessage(L"本地数据已缓存 (Todos:" + std::to_wstring(g_Todos.size())
                     + L", Countdowns:" + std::to_wstring(g_Countdowns.size()) + L")");
        }
    } catch (...) {
        LogMessage(L"SaveLocalData 失败");
    }
}

void LoadLocalData() {
    std::wstring cachePath = GetDataCachePath();
    HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogMessage(L"未找到本地数据缓存，首次启动或缓存已清除");
        return;
    }

    DWORD size = GetFileSize(hFile, NULL);
    if (size == 0 || size > 10 * 1024 * 1024) { // 超过 10MB 视为损坏
        CloseHandle(hFile);
        LogMessage(L"LoadLocalData：缓存文件异常，跳过");
        return;
    }

    std::string s(size, '\0');
    DWORD read;
    ReadFile(hFile, &s[0], size, &read, NULL);
    CloseHandle(hFile);

    try {
        auto root = json::parse(s);

        // 版本检查：低于 v2 的旧格式直接丢弃（字段不兼容）
        int version = root.value("version", 1);
        if (version < 2) {
            LogMessage(L"LoadLocalData：旧版缓存格式，忽略（将在下次同步后重建）");
            return;
        }

        // 用户 ID 校验：防止登录不同账号时加载到别人的数据
        int cachedUserId = root.value("user_id", 0);
        if (cachedUserId != g_UserId) {
            LogMessage(L"LoadLocalData：缓存 userId 不匹配，跳过");
            return;
        }

        std::vector<Todo>      tempTodos;
        std::vector<Countdown> tempCds;

        if (root.contains("todos") && root["todos"].is_array()) {
            for (const auto &j : root["todos"]) {
                Todo t;
                t.id          = j.value("id", 0);
                t.uuid        = ToWide(j.value("uuid", ""));
                t.content     = ToWide(j.value("content", ""));
                t.isDone      = j.value("is_done", false);
                t.isDirty     = j.value("is_dirty", false);
                t.lastUpdated = (time_t)(j.value("last_updated", 0LL) / 1000LL);

                long long createdMs = j.value("created_date_ms", 0LL);
                long long dueMs     = j.value("due_date_ms", 0LL);
                t.createdDate = UtcMsToDateString(createdMs);
                t.dueDate     = UtcMsToDateString(dueMs);

                tempTodos.push_back(t);
            }
        }

        if (root.contains("countdowns") && root["countdowns"].is_array()) {
            for (const auto &j : root["countdowns"]) {
                Countdown c;
                c.id          = j.value("id", 0);
                c.uuid        = ToWide(j.value("uuid", ""));
                c.title       = ToWide(j.value("title", ""));
                c.isDirty     = j.value("is_dirty", false);
                c.lastUpdated = (time_t)(j.value("last_updated", 0LL) / 1000LL);

                long long targetMs = j.value("target_time_ms", 0LL);
                c.dateStr  = UtcMsToDateOnly(targetMs);
                c.daysLeft = CalculateDaysLeft(c.dateStr);

                tempCds.push_back(c);
            }
        }

        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            g_Todos      = tempTodos;
            g_Countdowns = tempCds;
        }

        LogMessage(L"本地缓存加载完成 (Todos:" + std::to_wstring(tempTodos.size())
                 + L", Countdowns:" + std::to_wstring(tempCds.size()) + L")");

    } catch (...) {
        LogMessage(L"LoadLocalData：JSON 解析失败，缓存可能已损坏");
    }
}

std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "ERROR: Session failed";

    HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return "ERROR: Connect failed"; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "ERROR: Request failed";
    }

    // 连接超时 + 接收超时
    DWORD connectTimeout = 8000;
    DWORD receiveTimeout = 15000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receiveTimeout, sizeof(receiveTimeout));

    // 🚀 统一请求头：优先使用 Bearer Token，无 Token 时降级到旧 x-user-id
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!g_AuthToken.empty()) {
        headers += L"Authorization: Bearer " + g_AuthToken + L"\r\n";
    } else if (g_UserId > 0) {
        // 兼容未登录场景（如更新检查）
        headers += L"x-user-id: " + std::to_wstring(g_UserId) + L"\r\n";
    }

    if (WinHttpSendRequest(hRequest, headers.c_str(), -1,
            (LPVOID)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;
                char *buf = new char[dwSize + 1];
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, (LPVOID)buf, dwSize, &dwDownloaded)) {
                    buf[dwDownloaded] = '\0';
                    response += buf;
                }
                delete[] buf;
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
    j["email"]    = ToUtf8(email);
    j["password"] = ToUtf8(password);
    std::string res = SendRequest(L"/api/auth/login", "POST", j.dump());
    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            g_UserId   = resp["user"]["id"].get<int>();
            g_Username = ToWide(resp["user"]["username"].get<std::string>());
            if (resp["user"].contains("tier"))
                g_UserTier = ToWide(resp["user"]["tier"].get<std::string>());

            // 🚀 保存 Bearer Token
            if (resp.contains("token"))
                g_AuthToken = ToWide(resp["token"].get<std::string>());

            return "SUCCESS";
        }
        if (resp.contains("error")) return resp["error"].get<std::string>();
    } catch (...) {}
    return "FAILED";
}

bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    return ApiLogin(g_SavedEmail, g_SavedPass) == "SUCCESS";
}

/**
 * 🚀 模块 B：本地乐观写操作（不单独发网络请求，标记 isDirty 待批量上传）
 *
 * 设计说明：
 *   新后端的所有写入均通过 /api/sync POST 批量提交。
 *   因此 Add/Toggle/Delete 只在内存中操作并设 isDirty=true，
 *   由下一次 SyncData() 调用时统一携带上传。
 *   UI 层在调用这些函数后仍需调用 SyncData() 触发实际同步。
 */

 // 内部辅助：生成本地临时 UUID（格式与后端兼容）
static std::wstring GenLocalUuid() {
    srand((unsigned int)(time(nullptr) ^ (uintptr_t)GetCurrentThreadId()));
    wchar_t buf[40];
    swprintf_s(buf,
        L"pc%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
        (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF,
        (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF,
        (unsigned)rand()&0xFFFF,
        (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF);
    return buf;
}

void ApiAddTodo(const std::wstring &content, const std::wstring &createdDate,
                const std::wstring &dueDate, bool isDone) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    Todo t;
    t.id          = -(int)(g_Todos.size() + 1); // 负数临时 ID，服务器分配后替换
    t.uuid        = GenLocalUuid();
    t.content     = content;
    t.isDone      = isDone;
    t.lastUpdated = time(nullptr);
    t.createdDate = createdDate;
    t.dueDate     = dueDate;
    t.isDirty     = true;
    g_Todos.push_back(t);
    LogMessage(L"本地新增待办（待同步）: " + content);
}

void ApiToggleTodo(int id, bool done) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (t.id == id) {
            t.isDone      = done;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地切换待办状态（待同步）: " + t.content);
            return;
        }
    }
}

void ApiToggleTodoByUuid(const std::wstring &uuid, bool done) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (!uuid.empty() && t.uuid == uuid) {
            t.isDone      = done;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地切换待办状态 by uuid（待同步）: " + t.content);
            return;
        }
    }
}

void ApiUpdateTodo(const std::wstring &uuid, const std::wstring &content,
                   const std::wstring &createdDate, const std::wstring &dueDate, bool isDone) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        // 优先用 uuid 匹配，兼容 id 匹配（旧数据 uuid 可能为空）
        if ((!uuid.empty() && t.uuid == uuid) || (uuid.empty() && t.id == 0)) {
            t.content     = content;
            t.createdDate = createdDate;
            t.dueDate     = dueDate;
            t.isDone      = isDone;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地更新待办（待同步）: " + content);
            return;
        }
    }
    LogMessage(L"ApiUpdateTodo: 未找到 uuid=" + uuid + L"，降级为新增");
    // 未找到时降级为新增（容错）
    ApiAddTodo(content, createdDate, dueDate, isDone);
}

void ApiDeleteTodo(int id) {
    // 软删除：用特殊标记，SyncData 时携带 is_deleted=1 上传
    // 为避免复杂度，直接从内存移除；服务器端若 uuid 找不到则忽略
    // 对于已有 uuid 的条目，需发送一条 is_deleted=1 的 delta，
    // 故这里将其标记删除而非立即移除
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (t.id == id) {
            t.isDone      = false;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            // 用特殊 content 标记（SyncData 检测到此 flag 后以 is_deleted=1 上传）
            // 实际上直接从列表里移除并加入"待删除队列"更干净，
            // 简化处理：给 todo 附加一个删除标记字段
            // 由于结构体已有限，我们暂时把 id 取负号作为删除信号
            t.id = -(abs(t.id)); // 负数 = 标记为待删除
            LogMessage(L"本地标记待办删除（待同步）: " + t.content);
            break;
        }
    }
}

void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    Countdown c;
    c.id          = -(int)(g_Countdowns.size() + 1);
    c.uuid        = GenLocalUuid();
    c.title       = title;
    c.dateStr     = dateStr;
    c.daysLeft    = CalculateDaysLeft(dateStr);
    c.lastUpdated = time(nullptr);
    c.isDirty     = true;
    g_Countdowns.push_back(c);
    LogMessage(L"本地新增倒计时（待同步）: " + title);
}

void ApiDeleteCountdown(int id) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &c : g_Countdowns) {
        if (c.id == id) {
            c.lastUpdated = time(nullptr);
            c.isDirty     = true;
            c.id          = -(abs(c.id)); // 负数 = 标记为待删除
            LogMessage(L"本地标记倒计时删除（待同步）: " + c.title);
            break;
        }
    }
}

/**
 * 🚀 模块 C：Delta Sync 核心 —— /api/sync (POST)
 */
void SyncData() {
    if (g_UserId <= 0 || g_AuthToken.empty()) {
        LogMessage(L"SyncData 跳过：未登录或无 Token");
        return;
    }

    LogMessage(L"开始 Delta Sync (last_sync_time=" + std::to_wstring(g_LastSyncTime) + L")...");

    // ── 1. 构建本地变更包（dirty items）──
    json todosChanges   = json::array();
    json countdownsChanges = json::array();

    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

        for (const auto &t : g_Todos) {
            if (!t.isDirty) continue;

            bool isDeleted = (t.id < 0 && !t.uuid.empty());
            json item;
            item["uuid"]         = ToUtf8(t.uuid);
            item["content"]      = ToUtf8(t.content);
            item["is_completed"] = t.isDone ? 1 : 0;
            item["is_deleted"]   = isDeleted ? 1 : 0;
            item["updated_at"]   = (long long)t.lastUpdated * 1000LL;
            item["version"]      = 1;

            // 日期字段：转为 UTC ms 时间戳
            long long createdMs = DateStringToUtcMs(t.createdDate);
            long long dueMs     = DateStringToUtcMs(t.dueDate);
            if (createdMs > 0) item["created_date"] = createdMs;
            else               item["created_date"] = nullptr;
            if (dueMs > 0)     item["due_date"]     = dueMs;
            else               item["due_date"]     = nullptr;

            item["created_at"] = createdMs > 0 ? createdMs : (long long)t.lastUpdated * 1000LL;
            todosChanges.push_back(item);
        }

        for (const auto &c : g_Countdowns) {
            if (!c.isDirty) continue;

            bool isDeleted = (c.id < 0 && !c.uuid.empty());
            json item;
            item["uuid"]       = ToUtf8(c.uuid);
            item["title"]      = ToUtf8(c.title);
            item["is_deleted"] = isDeleted ? 1 : 0;
            item["updated_at"] = (long long)c.lastUpdated * 1000LL;
            item["version"]    = 1;
            item["created_at"] = (long long)c.lastUpdated * 1000LL;

            // target_time：把日期字符串转为 UTC ms
            long long targetMs = DateStringToUtcMs(c.dateStr);
            if (targetMs > 0) item["target_time"] = targetMs;
            else              item["target_time"] = nullptr;

            countdownsChanges.push_back(item);
        }
    }

    // ── 2. 屏幕时间（从 g_AppUsage 读取今日本机数据）──
    json screenTimePayload = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        json apps = json::array();
        for (const auto &rec : g_AppUsage) {
            // 只上传本机数据（device_name 与本机一致的条目）
            if (rec.deviceName != g_DeviceName) continue;
            json a;
            a["app_name"] = ToUtf8(rec.appName);
            a["duration"] = rec.seconds;
            apps.push_back(a);
        }
        if (!apps.empty()) {
            screenTimePayload = json::object();
            screenTimePayload["device_name"] = ToUtf8(g_DeviceName);
            // record_date：今日日期字符串 YYYY-MM-DD
            screenTimePayload["record_date"] = ToUtf8(GetTodayDate());
            screenTimePayload["apps"]        = apps;
        }
    }

    // ── 3. 组装完整 Payload ──
    json payload;
    payload["user_id"]        = g_UserId;
    payload["device_id"]      = ToUtf8(g_DeviceId);
    payload["last_sync_time"] = g_LastSyncTime;
    payload["todos"]          = todosChanges;
    payload["countdowns"]     = countdownsChanges;
    if (!screenTimePayload.is_null()) {
        payload["screen_time"] = screenTimePayload;
    }

    // ── 4. 发送请求 ──
    std::string res = SendRequest(L"/api/sync", "POST", payload.dump());

    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"Delta Sync 失败: " + ToWide(res));
        return;
    }

    // ── 5. 解析响应并合并到本地 ──
    try {
        auto resp = json::parse(res);
        if (!resp.contains("success") || !resp["success"].get<bool>()) {
            std::string errMsg = resp.contains("error") ? resp["error"].get<std::string>() : "unknown";
            LogMessage(L"Delta Sync 服务器拒绝: " + ToWide(errMsg));
            return;
        }

        // 更新同步限额状态
        if (resp.contains("status")) {
            auto &st = resp["status"];
            if (st.contains("tier"))       g_UserTier  = ToWide(st["tier"].get<std::string>());
            if (st.contains("sync_count")) g_SyncCount = st["sync_count"].get<int>();
            if (st.contains("sync_limit")) g_SyncLimit = st["sync_limit"].get<int>();
            SaveSyncStatusToLocal();
        }

        // 更新并持久化 last_sync_time
        if (resp.contains("new_sync_time")) {
            g_LastSyncTime = resp["new_sync_time"].get<long long>();
            SaveLastSyncTime(g_LastSyncTime);
        }

        // ── 5a. 合并服务器下发的 Todos ──
        if (resp.contains("server_todos") && resp["server_todos"].is_array()) {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

            for (auto &st : resp["server_todos"]) {
                bool sDeleted = false;
                if (st.contains("is_deleted")) {
                    auto v = st["is_deleted"];
                    sDeleted = v.is_number() ? (v.get<int>() != 0) : v.get<bool>();
                }

                std::wstring sUuid = ToWide(st.value("uuid", st.value("id", "")));
                if (sUuid.empty()) continue;

                // 从服务器时间戳转回日期字符串
                long long createdDateMs = st.contains("created_date") && !st["created_date"].is_null()
                    ? st["created_date"].get<long long>() : 0LL;
                long long dueDateMs = st.contains("due_date") && !st["due_date"].is_null()
                    ? st["due_date"].get<long long>() : 0LL;

                std::wstring sCreatedDate = UtcMsToDateString(createdDateMs);
                std::wstring sDueDate     = UtcMsToDateString(dueDateMs);
                std::wstring sContent     = ToWide(st.value("content", ""));
                bool sCompleted = st.contains("is_completed")
                    ? (st["is_completed"].is_number()
                        ? st["is_completed"].get<int>() != 0
                        : st["is_completed"].get<bool>())
                    : false;

                // 在本地列表中查找匹配的条目（先按 uuid，再按 content 模糊）
                auto it = std::find_if(g_Todos.begin(), g_Todos.end(),
                    [&](const Todo &t){ return t.uuid == sUuid; });

                if (sDeleted) {
                    // 服务器标记删除 → 本地移除
                    if (it != g_Todos.end()) g_Todos.erase(it);
                    continue;
                }

                if (it != g_Todos.end()) {
                    // 已存在：仅当本地未 dirty 时才用服务器数据覆盖（避免覆盖用户正在编辑的内容）
                    if (!it->isDirty) {
                        it->content     = sContent;
                        it->isDone      = sCompleted;
                        it->createdDate = sCreatedDate;
                        it->dueDate     = sDueDate;
                    }
                } else {
                    // 不存在：插入新条目
                    Todo t;
                    t.id          = st.contains("id") && st["id"].is_number()
                                    ? st["id"].get<int>() : 0;
                    t.uuid        = sUuid;
                    t.content     = sContent;
                    t.isDone      = sCompleted;
                    t.createdDate = sCreatedDate;
                    t.dueDate     = sDueDate;
                    t.lastUpdated = time(nullptr);
                    t.isDirty     = false;
                    g_Todos.push_back(t);
                }
            }

            // 清除已成功上传的 dirty 标记，并移除本地软删除条目
            g_Todos.erase(std::remove_if(g_Todos.begin(), g_Todos.end(),
                [](const Todo &t){ return t.id < 0; }), g_Todos.end());
            for (auto &t : g_Todos) t.isDirty = false;

            LogMessage(L"Todos 合并完成，当前共 " + std::to_wstring(g_Todos.size()) + L" 条");
        }

        // ── 5b. 合并服务器下发的 Countdowns ──
        if (resp.contains("server_countdowns") && resp["server_countdowns"].is_array()) {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

            for (auto &sc : resp["server_countdowns"]) {
                bool sDeleted = false;
                if (sc.contains("is_deleted")) {
                    auto v = sc["is_deleted"];
                    sDeleted = v.is_number() ? (v.get<int>() != 0) : v.get<bool>();
                }

                std::wstring sUuid  = ToWide(sc.value("uuid", sc.value("id", "")));
                std::wstring sTitle = ToWide(sc.value("title", ""));
                long long targetMs  = sc.contains("target_time") && !sc["target_time"].is_null()
                    ? sc["target_time"].get<long long>() : 0LL;
                std::wstring sDateStr = UtcMsToDateOnly(targetMs);

                auto it = std::find_if(g_Countdowns.begin(), g_Countdowns.end(),
                    [&](const Countdown &c){ return c.uuid == sUuid; });

                if (sDeleted) {
                    if (it != g_Countdowns.end()) g_Countdowns.erase(it);
                    continue;
                }

                if (it != g_Countdowns.end()) {
                    if (!it->isDirty) {
                        it->title     = sTitle;
                        it->dateStr   = sDateStr;
                        it->daysLeft  = CalculateDaysLeft(sDateStr);
                    }
                } else {
                    Countdown c;
                    c.id          = sc.contains("id") && sc["id"].is_number()
                                    ? sc["id"].get<int>() : 0;
                    c.uuid        = sUuid;
                    c.title       = sTitle;
                    c.dateStr     = sDateStr;
                    c.daysLeft    = CalculateDaysLeft(sDateStr);
                    c.lastUpdated = time(nullptr);
                    c.isDirty     = false;
                    g_Countdowns.push_back(c);
                }
            }

            g_Countdowns.erase(std::remove_if(g_Countdowns.begin(), g_Countdowns.end(),
                [](const Countdown &c){ return c.id < 0; }), g_Countdowns.end());
            for (auto &c : g_Countdowns) c.isDirty = false;

            LogMessage(L"Countdowns 合并完成，当前共 " + std::to_wstring(g_Countdowns.size()) + L" 条");
        }

        // ── 5c. 同步后拉取聚合屏幕时间（仅 GET，用于展示多端数据）──
        {
            std::wstring today    = GetTodayDate();
            std::wstring getUrl   = L"/api/screen_time?user_id=" + std::to_wstring(g_UserId)
                                  + L"&date=" + today;
            std::string stRes = SendRequest(getUrl, "GET", "");
            if (!stRes.empty() && stRes.find("ERROR") != 0) {
                try {
                    auto jArr = json::parse(stRes);
                    if (jArr.is_array()) {
                        std::vector<AppUsageRecord> details;
                        for (const auto &item : jArr) {
                            AppUsageRecord r;
                            r.appName    = ToWide(item.value("app_name", ""));
                            r.deviceName = ToWide(item.value("device_name", ""));
                            r.seconds    = item.value("duration", 0);
                            details.push_back(r);
                        }
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        g_AppUsage = details;
                    }
                } catch (...) {}
            }
        }

        LogMessage(L"Delta Sync 完成!");
        // 🚀 合并完成后立即持久化到本地，下次重启可直接加载
        SaveLocalData();
        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);

    } catch (const std::exception &e) {
        LogMessage(L"Delta Sync 解析异常: " + ToWide(std::string(e.what())));
    } catch (...) {
        LogMessage(L"Delta Sync 未知异常");
    }
}

/**
 * 模块 D：ApiFetchUserStatus（保留兼容，用于右键菜单离线检测）
 * 注意：新后端已无 /api/user/status 端点，此函数改为从 SyncData 的
 *       status 字段更新，直接返回 true 即可（右键菜单离线检测降级处理）
 */
bool ApiFetchUserStatus() {
    // 新架构中 status 由 SyncData 响应携带更新，此函数保留以兼容调用方
    // 这里做一次轻量 SyncData（传空变更包，仅获取 status）
    if (g_UserId <= 0 || g_AuthToken.empty()) return false;

    json payload;
    payload["user_id"]        = g_UserId;
    payload["device_id"]      = ToUtf8(g_DeviceId);
    payload["last_sync_time"] = g_LastSyncTime;
    payload["todos"]          = json::array();
    payload["countdowns"]     = json::array();

    std::string res = SendRequest(L"/api/sync", "POST", payload.dump());
    if (res.empty() || res.find("ERROR") == 0) return false;

    try {
        auto resp = json::parse(res);
        if (resp.contains("status")) {
            auto &st = resp["status"];
            if (st.contains("tier"))       g_UserTier  = ToWide(st["tier"].get<std::string>());
            if (st.contains("sync_count")) g_SyncCount = st["sync_count"].get<int>();
            if (st.contains("sync_limit")) g_SyncLimit = st["sync_limit"].get<int>();
            SaveSyncStatusToLocal();
        }
        return true;
    } catch (...) { return false; }
}

/**
 * 模块 E：课程表（逻辑不变，但请求头已自动升级为 Bearer Token）
 */
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
            for (auto &it : jArr) {
                Course c;
                c.id          = it.value("id", 0);
                c.courseName  = ToWide(it.value("course_name", "未知课程"));
                c.roomName    = ToWide(it.value("room_name",   "未知教室"));
                c.teacherName = ToWide(it.value("teacher_name", ""));
                c.startTime   = it.value("start_time", 0);
                c.endTime     = it.value("end_time",   0);
                c.weekday     = it.value("weekday",    1);
                c.weekIndex   = it.value("week_index", 1);
                if (it.contains("lesson_type") && !it["lesson_type"].is_null())
                    c.lessonType = ToWide(it.value("lesson_type", ""));
                tempCourses.push_back(c);
            }
            {
                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                g_Courses = tempCourses;
            }
            SaveLocalCourses();
            LogMessage(L"课程表拉取并缓存成功! 共 " + std::to_wstring(tempCourses.size()) + L" 节课");
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
        } else if (jArr.is_object() && jArr.contains("error")) {
            LogMessage(L"课程表拉取被拒绝: " + ToWide(jArr["error"].get<std::string>()));
        }
    } catch (...) {
        LogMessage(L"课程表数据解析 JSON 失败");
    }
}

/**
 * 兼容旧接口：ApiSyncScreenTime（由 tai_reader 调用，保留签名）
 * 现在只做本地数据更新，实际上传由 SyncData 的 screen_time 字段携带
 */
std::map<std::wstring, int> ApiSyncScreenTime(
    const std::map<std::wstring, int> &localData,
    const std::wstring &dateStr,
    const std::wstring &deviceName)
{
    if (g_UserId <= 0) return localData;

    // 将本机屏幕时间写入 g_AppUsage（SyncData 会在下次同步时携带上传）
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        // 先移除旧的本机条目，再插入最新数据
        g_AppUsage.erase(std::remove_if(g_AppUsage.begin(), g_AppUsage.end(),
            [&](const AppUsageRecord &r){ return r.deviceName == deviceName; }),
            g_AppUsage.end());
        for (const auto &p : localData) {
            AppUsageRecord r;
            r.appName    = p.first;
            r.deviceName = deviceName;
            r.seconds    = p.second;
            g_AppUsage.push_back(r);
        }
    }

    // 返回当前聚合数据（含其他设备）
    std::map<std::wstring, int> agg;
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        for (const auto &r : g_AppUsage) agg[r.appName] += r.seconds;
    }
    return agg.empty() ? localData : agg;
}

